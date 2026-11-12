// Explicit GLES2 shader/resource subset. No global Mesa replacement.
static int shader_index(GLuint name){if(!active||!name||name>LR_GL_MAX_SHADERS||!active->shader[name-1].name){gl_error(GL_INVALID_VALUE);return -1;}return name-1;}
static int program_index(GLuint name){if(!active||name<65||name>=65+LR_GL_MAX_PROGRAMS||!active->program[name-65].name){gl_error(GL_INVALID_VALUE);return -1;}return name-65;}
static void shader_release(unsigned index){
 if(active->shader[index].deleted&&!active->shader[index].refs){struct lr_gpu_request r={.operation=LR_GPU_DELETE_SHADER,.count=index+1};submit(&r);free(active->shader[index].source);memset(&active->shader[index],0,sizeof active->shader[index]);}
}
GLuint glCreateShader(GLenum stage){
 if(!active||(active->client_version!=2&&!active->desktop)){gl_error(GL_INVALID_OPERATION);return 0;}if(stage!=GL_VERTEX_SHADER&&stage!=GL_FRAGMENT_SHADER){gl_error(GL_INVALID_ENUM);return 0;}
 for(unsigned i=0;i<LR_GL_MAX_SHADERS;i++)if(!active->shader[i].name){active->shader[i].name=i+1;active->shader[i].stage=stage;return i+1;}gl_error(GL_OUT_OF_MEMORY);return 0;
}
void glShaderSource(GLuint name,GLsizei count,const GLchar *const *strings,const GLint *lengths){
 int i=shader_index(name);if(i<0)return;if(count<0||count>32||(!strings&&count)){gl_error(GL_INVALID_VALUE);return;}
 size_t size=0;for(int k=0;k<count;k++){if(!strings[k]){gl_error(GL_INVALID_VALUE);return;}size_t n=lengths&&lengths[k]>=0?(size_t)lengths[k]:strnlen(strings[k],LR_GL_MAX_SOURCE);if(n>=LR_GL_MAX_SOURCE-size){gl_error(GL_INVALID_VALUE);return;}size+=n;}
 char *source=malloc(size+1);if(!source){gl_error(GL_OUT_OF_MEMORY);return;}size_t at=0;for(int k=0;k<count;k++){size_t n=lengths&&lengths[k]>=0?(size_t)lengths[k]:strlen(strings[k]);memcpy(source+at,strings[k],n);at+=n;}source[size]=0;
 if(memchr(source,0,size)){free(source);gl_error(GL_INVALID_VALUE);return;}free(active->shader[i].source);active->shader[i].source=source;
}
void glCompileShader(GLuint name){
 int i=shader_index(name);if(i<0)return;struct lr_gl_compile packet={.name=name,.stage=active->shader[i].stage};
 if(active->shader[i].source){packet.length=strlen(active->shader[i].source);memcpy(packet.source,active->shader[i].source,packet.length+1);}
 struct lr_gpu_request r={.operation=LR_GPU_COMPILE_SHADER_EXTENDED,.address=(uintptr_t)&packet,.bytes=sizeof packet};active->shader[i].compiled=submit(&r)&&packet.status;memcpy(active->shader[i].log,packet.log,sizeof packet.log);
}
static void copy_log(const char *log,GLsizei size,GLsizei *length,GLchar *out){if(size<0||(!out&&size)){gl_error(GL_INVALID_VALUE);return;}size_t n=size?(size_t)size-1:0;if(n>strlen(log))n=strlen(log);if(size){memcpy(out,log,n);out[n]=0;}if(length)*length=n;}
void glGetShaderInfoLog(GLuint name,GLsizei size,GLsizei *length,GLchar *out){int i=shader_index(name);if(i>=0)copy_log(active->shader[i].log,size,length,out);}
void glGetShaderiv(GLuint name,GLenum property,GLint *out){int i=shader_index(name);if(i<0)return;if(!out){gl_error(GL_INVALID_VALUE);return;}switch(property){case GL_COMPILE_STATUS:*out=active->shader[i].compiled;break;case GL_SHADER_TYPE:*out=active->shader[i].stage;break;case GL_DELETE_STATUS:*out=active->shader[i].deleted;break;case GL_INFO_LOG_LENGTH:*out=strlen(active->shader[i].log)+1;break;case GL_SHADER_SOURCE_LENGTH:*out=active->shader[i].source?strlen(active->shader[i].source)+1:0;break;default:gl_error(GL_INVALID_ENUM);}}
void glDeleteShader(GLuint name){if(!name)return;int i=shader_index(name);if(i<0)return;active->shader[i].deleted=1;shader_release(i);}
GLuint glCreateProgram(void){if(!active||(active->client_version!=2&&!active->desktop)){gl_error(GL_INVALID_OPERATION);return 0;}for(unsigned i=0;i<LR_GL_MAX_PROGRAMS;i++)if(!active->program[i].name){active->program[i].name=65+i;return 65+i;}gl_error(GL_OUT_OF_MEMORY);return 0;}
void glAttachShader(GLuint name,GLuint shader){int p=program_index(name),s=shader_index(shader);if(p<0||s<0)return;unsigned *slot=active->shader[s].stage==GL_VERTEX_SHADER?&active->program[p].vertex:&active->program[p].fragment;if(*slot){gl_error(GL_INVALID_OPERATION);return;}*slot=shader;active->shader[s].refs++;}
void glDetachShader(GLuint name,GLuint shader){int p=program_index(name),s=shader_index(shader);if(p<0||s<0)return;unsigned *slot=active->shader[s].stage==GL_VERTEX_SHADER?&active->program[p].vertex:&active->program[p].fragment;if(*slot!=shader){gl_error(GL_INVALID_OPERATION);return;}*slot=0;active->shader[s].refs--;shader_release(s);}
void glBindAttribLocation(GLuint name,GLuint location,const GLchar *attribute){int p=program_index(name);if(p<0)return;if(location>=LR_GL_MAX_ATTRIBUTES||!attribute||strnlen(attribute,64)>=64){gl_error(GL_INVALID_VALUE);return;}if(!strncmp(attribute,"gl_",3)){gl_error(GL_INVALID_OPERATION);return;}
 unsigned i=0;while(i<active->program[p].binding_count&&strcmp(active->program[p].bindings[i].name,attribute))i++;if(i==LR_GL_MAX_ATTRIBUTES){gl_error(GL_OUT_OF_MEMORY);return;}if(i==active->program[p].binding_count)active->program[p].binding_count++;strcpy(active->program[p].bindings[i].name,attribute);active->program[p].bindings[i].location=location;
}
// Array locations occupy disjoint contiguous ranges. Existing scalar locations
// and the original draw packet remain stable for old subset applications.
static int uniform_slot(unsigned p,GLint location,unsigned *element){
 unsigned index;if(location<0)return -1;
 if(location<LR_GL_MAX_UNIFORMS){index=location;*element=0;}
 else{index=((unsigned)location-LR_GL_MAX_UNIFORMS)/LR_GL_MAX_UNIFORM_ELEMENTS;*element=((unsigned)location-LR_GL_MAX_UNIFORMS)%LR_GL_MAX_UNIFORM_ELEMENTS;}
 if(index>=active->program[p].executable.uniforms||*element>=active->program[p].extents.uniform[index].elements)return -1;
 if((location>=LR_GL_MAX_UNIFORMS)!=(active->program[p].extents.uniform[index].elements>1))return -1;
 return index;
}
static int uniform_array_update(unsigned p,unsigned index,unsigned first,GLsizei count,const void *values){
 unsigned elements=active->program[p].extents.uniform[index].elements,type=active->program[p].executable.uniform[index].type,n=lr_gl_type_components(type);
 if(count<0||(!values&&count)){gl_error(GL_INVALID_VALUE);return 0;}
 if(!count)return 1;
 // OpenGL ignores updates beyond the end of an array; scalar count>1 is invalid.
 if(elements==1&&count>1){gl_error(GL_INVALID_OPERATION);return 0;}
 unsigned take=(unsigned)count;if(take>elements-first)take=elements-first;
 size_t bytes=(size_t)take*n*4,capacity=(size_t)elements*n*4;
 if(!active->program[p].array_values[index]){active->program[p].array_values[index]=calloc(1,capacity);if(!active->program[p].array_values[index]){gl_error(GL_OUT_OF_MEMORY);return 0;}}
 struct lr_gl_uniform_array_data *packet=malloc(sizeof(*packet)+bytes);if(!packet){gl_error(GL_OUT_OF_MEMORY);return 0;}
 packet->program=p+1;packet->index=index;packet->first=first;packet->count=take;memcpy(packet->data,values,bytes);
 if(lr_gl_type_kind(type)==3)for(unsigned k=0;k<take*n;k++)packet->data[k]=!!packet->data[k];
 struct lr_gpu_request r={.operation=LR_GPU_UNIFORM_ARRAY_DATA,.address=(uintptr_t)packet,.bytes=sizeof(*packet)+bytes};int ok=submit(&r);
 if(ok)memcpy(active->program[p].array_values[index]+(size_t)first*n*4,packet->data,bytes);free(packet);return ok;
}
void glLinkProgram(GLuint name){int p=program_index(name);if(p<0)return;struct lr_gl_link packet={.name=p+1,.vertex=active->program[p].vertex,.fragment=active->program[p].fragment,.attributes=active->program[p].binding_count};memcpy(packet.attribute,active->program[p].bindings,sizeof packet.attribute);
 if(!packet.vertex||!packet.fragment||!active->shader[packet.vertex-1].compiled||!active->shader[packet.fragment-1].compiled){active->program[p].link.status=0;strcpy(active->program[p].link.log,"Missing compiled vertex/fragment shader");return;}
 struct lr_gpu_request r={.operation=LR_GPU_LINK_PROGRAM,.address=(uintptr_t)&packet,.bytes=sizeof packet};if(!submit(&r))packet.status=0;active->program[p].link=packet;
 if(packet.status){struct lr_gl_uniform_extents extents={.program=p+1};struct lr_gpu_request uq={.operation=LR_GPU_QUERY_UNIFORM_EXTENTS,.address=(uintptr_t)&extents,.bytes=sizeof extents};if(!submit(&uq)){active->program[p].link.status=0;strcpy(active->program[p].link.log,"Default uniform reflection unavailable");return;}struct lr_gl_storages storages={.program=p+1};struct lr_gpu_request sq={.operation=LR_GPU_QUERY_STORAGES,.address=(uintptr_t)&storages,.bytes=sizeof storages};if(!submit(&sq)){active->program[p].link.status=0;strcpy(active->program[p].link.log,"Storage block reflection unavailable");return;}active->program[p].storages=storages;active->program[p].storage_valid=0;for(unsigned s=0;s<storages.count;s++)active->program[p].storage_binding[s]=storages.block[s].binding;active->program[p].extents=extents;for(unsigned u=0;u<LR_GL_MAX_UNIFORMS;u++){free(active->program[p].array_values[u]);active->program[p].array_values[u]=NULL;}struct lr_gl_blocks *blocks=calloc(1,sizeof *blocks);if(!blocks){active->program[p].link.status=0;gl_error(GL_OUT_OF_MEMORY);return;}blocks->program=p+1;struct lr_gpu_request query={.operation=LR_GPU_QUERY_BLOCKS_EXTENDED,.address=(uintptr_t)blocks,.bytes=sizeof *blocks};if(!submit(&query)){free(blocks);active->program[p].link.status=0;strcpy(active->program[p].link.log,"Uniform block reflection unavailable");return;}free(active->program[p].blocks);active->program[p].blocks=blocks;for(unsigned i=0;i<blocks->count;i++)active->program[p].block_binding[i]=blocks->block[i].binding;active->program[p].executable=packet;active->program[p].ubo_valid=0;active->program[p].can_draw=1;memset(active->program[p].values,0,sizeof active->program[p].values);memset(active->program[p].sampler_unit,0,sizeof active->program[p].sampler_unit);}
}
// Reflection is taken from the successful compiler link; never synthesize resources.
static unsigned active_name_max(const struct lr_gl_variable *v,unsigned count){unsigned n=0;for(unsigned i=0;i<count;i++){unsigned k=strlen(v[i].name)+1;if(k>n)n=k;}return n;}
static void active_variable(GLuint name,GLuint index,GLsizei capacity,GLsizei *length,GLint *size,GLenum *type,GLchar *out,int uniform){
 int p=program_index(name);if(p<0)return;
 if(capacity<0||(!out&&capacity)||!size||!type){gl_error(GL_INVALID_VALUE);return;}
 unsigned count=uniform?active->program[p].link.uniforms:active->program[p].link.attributes;
 if(uniform&&index>=count){const struct lr_gl_block_member *m=block_member(p,index,NULL);if(!m){gl_error(GL_INVALID_VALUE);return;}*size=m->size;*type=m->type;copy_log(m->name,capacity,length,out);return;}
 if(index>=count){gl_error(GL_INVALID_VALUE);return;}
 const struct lr_gl_variable *v=uniform?&active->program[p].link.uniform[index]:&active->program[p].link.attribute[index];
 *size=uniform?active->program[p].extents.uniform[index].elements:1;*type=v->type;
 if(uniform&&*size>1){char name[68];snprintf(name,sizeof name,"%s[0]",v->name);copy_log(name,capacity,length,out);}else copy_log(v->name,capacity,length,out);
}
void glGetActiveUniform(GLuint p,GLuint i,GLsizei n,GLsizei *l,GLint *s,GLenum *t,GLchar *out){active_variable(p,i,n,l,s,t,out,1);}
void glGetActiveAttrib(GLuint p,GLuint i,GLsizei n,GLsizei *l,GLint *s,GLenum *t,GLchar *out){active_variable(p,i,n,l,s,t,out,0);}
void glGetAttachedShaders(GLuint name,GLsizei capacity,GLsizei *count,GLuint *out){
 int p=program_index(name);if(p<0)return;if(capacity<0||(!out&&capacity)){gl_error(GL_INVALID_VALUE);return;}
 GLsizei n=0;if(active->program[p].vertex&&n<capacity)out[n++]=active->program[p].vertex;
 if(active->program[p].fragment&&n<capacity)out[n++]=active->program[p].fragment;if(count)*count=n;
}
void glGetShaderSource(GLuint name,GLsizei capacity,GLsizei *length,GLchar *out){int i=shader_index(name);if(i>=0)copy_log(active->shader[i].source?active->shader[i].source:"",capacity,length,out);}
void glGetProgramiv(GLuint name,GLenum property,GLint *out){int p=program_index(name);if(p<0)return;if(!out){gl_error(GL_INVALID_VALUE);return;}switch(property){case GL_LINK_STATUS:*out=active->program[p].link.status;break;case GL_DELETE_STATUS:*out=active->program[p].deleted;break;case GL_INFO_LOG_LENGTH:*out=strlen(active->program[p].link.log)+1;break;case GL_ATTACHED_SHADERS:*out=(active->program[p].vertex!=0)+(active->program[p].fragment!=0);break;case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:*out=active_name_max(active->program[p].link.attribute,active->program[p].link.attributes);break;case GL_ACTIVE_UNIFORM_MAX_LENGTH:*out=active_name_max(active->program[p].link.uniform,active->program[p].link.uniforms);for(unsigned i=0;i<active->program[p].link.uniforms;i++)if(active->program[p].extents.uniform[i].elements>1&&(GLint)strlen(active->program[p].link.uniform[i].name)+4>*out)*out=strlen(active->program[p].link.uniform[i].name)+4;if(block_member_name_max(p)>*out)*out=block_member_name_max(p);break;case GL_ACTIVE_ATTRIBUTES:*out=active->program[p].link.attributes;break;case GL_ACTIVE_UNIFORMS:*out=active->program[p].link.uniforms+block_member_count(p);break;case 0x8a36:*out=active->program[p].blocks?active->program[p].blocks->count:0;break;case 0x8a35:{*out=0;for(unsigned i=0;active->program[p].blocks&&i<active->program[p].blocks->count;i++){unsigned n=strlen(active->program[p].blocks->block[i].name)+1;if(n>*out)*out=n;}break;}default:gl_error(GL_INVALID_ENUM);}}
void glGetProgramInfoLog(GLuint name,GLsizei size,GLsizei *length,GLchar *out){int p=program_index(name);if(p>=0)copy_log(active->program[p].link.log,size,length,out);}
static void program_release(unsigned p){unsigned v=active->program[p].vertex,f=active->program[p].fragment;if(v){active->shader[v-1].refs--;shader_release(v-1);}if(f){active->shader[f-1].refs--;shader_release(f-1);}struct lr_gpu_request r={.operation=LR_GPU_DELETE_PROGRAM,.count=p+1};submit(&r);free(active->program[p].blocks);for(unsigned u=0;u<LR_GL_MAX_UNIFORMS;u++)free(active->program[p].array_values[u]);memset(&active->program[p],0,sizeof active->program[p]);}
void glUseProgram(GLuint name){int p=name?program_index(name):-1;if(name&&(p<0||!active->program[p].link.status)){if(p>=0)gl_error(GL_INVALID_OPERATION);return;}if(!active){gl_error(GL_INVALID_OPERATION);return;}
 unsigned old=active->active_program;active->active_program=name?p+1:0;if(old&&old!=active->active_program&&active->program[old-1].deleted)program_release(old-1);
}
void glDeleteProgram(GLuint name){if(!name)return;int p=program_index(name);if(p<0)return;active->program[p].deleted=1;if(active->active_program!=p+1)program_release(p);}
GLint glGetUniformLocation(GLuint name,const GLchar *uniform){int p=program_index(name);if(p<0||!uniform)return -1;if(!active->program[p].link.status){gl_error(GL_INVALID_OPERATION);return -1;}
 for(unsigned i=0;i<active->program[p].link.uniforms;i++){const char *base=active->program[p].link.uniform[i].name;size_t n=strlen(base);unsigned elements=active->program[p].extents.uniform[i].elements,element=0;
  if(strncmp(uniform,base,n))continue;
  if(uniform[n]){if(elements<2||uniform[n]!='['||uniform[n+1]<'0'||uniform[n+1]>'9')continue;char *end;unsigned long value=strtoul(uniform+n+1,&end,10);if(*end!=']'||end[1]||value>=elements)continue;element=value;}
  return elements>1?LR_GL_MAX_UNIFORMS+i*LR_GL_MAX_UNIFORM_ELEMENTS+element:i;
 }return -1;
}
GLint glGetAttribLocation(GLuint name,const GLchar *attribute){int p=program_index(name);if(p<0||!attribute)return -1;if(!active->program[p].link.status){gl_error(GL_INVALID_OPERATION);return -1;}for(unsigned i=0;i<active->program[p].link.attributes;i++)if(!strcmp(attribute,active->program[p].link.attribute[i].name))return active->program[p].link.attribute[i].location;return -1;}
void glGetUniformfv(GLuint name,GLint location,GLfloat *out){uniform_query(name,location,out,0);}
static void uniform_value(GLint location,GLsizei count,GLenum type,const GLfloat *values,unsigned components){if(location==-1)return;if(!active||!active->active_program){gl_error(GL_INVALID_OPERATION);return;}unsigned p=active->active_program-1,first=0;int slot=uniform_slot(p,location,&first);
 if(slot<0||(active->program[p].executable.uniform[slot].type!=type&&!(lr_gl_type_kind(active->program[p].executable.uniform[slot].type)==3&&lr_gl_type_components(active->program[p].executable.uniform[slot].type)==components))){gl_error(GL_INVALID_OPERATION);return;}
 if(active->program[p].extents.uniform[slot].elements>1){
  if(lr_gl_type_kind(active->program[p].executable.uniform[slot].type)==3){if(count<0||count>LR_GL_MAX_UNIFORM_ELEMENTS||(!values&&count)){gl_error(GL_INVALID_VALUE);return;}uint32_t bools[LR_GL_MAX_UNIFORM_ELEMENTS*4];for(unsigned i=0;i<(unsigned)count*components;i++)bools[i]=values[i]!=0;uniform_array_update(p,slot,first,count,bools);}
  else uniform_array_update(p,slot,first,count,values);return;
 }
 if(count<0||(!values&&count)){gl_error(GL_INVALID_VALUE);return;}if(count>1){gl_error(GL_INVALID_OPERATION);return;}if(count){if(lr_gl_type_kind(active->program[p].executable.uniform[slot].type)==3){for(unsigned i=0;i<components;i++){uint32_t b=values[i]!=0;memcpy(&active->program[p].values[slot][i],&b,4);}}else memcpy(active->program[p].values[slot],values,components*4);}
}
void glUniform1fv(GLint l,GLsizei n,const GLfloat *v){uniform_value(l,n,GL_FLOAT,v,1);}void glUniform2fv(GLint l,GLsizei n,const GLfloat *v){uniform_value(l,n,GL_FLOAT_VEC2,v,2);}void glUniform3fv(GLint l,GLsizei n,const GLfloat *v){uniform_value(l,n,GL_FLOAT_VEC3,v,3);}void glUniform4fv(GLint l,GLsizei n,const GLfloat *v){uniform_value(l,n,GL_FLOAT_VEC4,v,4);}
void glUniform1f(GLint l,GLfloat x){glUniform1fv(l,1,&x);}void glUniform2f(GLint l,GLfloat x,GLfloat y){float v[]={x,y};glUniform2fv(l,1,v);}void glUniform3f(GLint l,GLfloat x,GLfloat y,GLfloat z){float v[]={x,y,z};glUniform3fv(l,1,v);}void glUniform4f(GLint l,GLfloat x,GLfloat y,GLfloat z,GLfloat w){float v[]={x,y,z,w};glUniform4fv(l,1,v);}
void glUniformMatrix4fv(GLint l,GLsizei count,GLboolean transpose,const GLfloat *v){if(transpose){gl_error(GL_INVALID_VALUE);return;}uniform_value(l,count,GL_FLOAT_MAT4,v,16);}
void glVertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void *pointer){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(index>=LR_GL_MAX_ATTRIBUTES||size<1||size>4||stride<0){gl_error(GL_INVALID_VALUE);return;}unsigned bytes=type==GL_FLOAT?4:(type==GL_SHORT||type==GL_UNSIGNED_SHORT)?2:(type==GL_BYTE||type==GL_UNSIGNED_BYTE)?1:0;if(!bytes){gl_error(GL_INVALID_ENUM);return;}
 active->attribute[index].integer=0;active->attribute[index].size=size;active->attribute[index].type=type;active->attribute[index].normalized=normalized;active->attribute[index].stride=stride?stride:size*bytes;active->attribute[index].buffer=active->bound_buffer;active->attribute[index].pointer=pointer;
}
void glEnableVertexAttribArray(GLuint i){if(!active||i>=LR_GL_MAX_ATTRIBUTES){gl_error(GL_INVALID_VALUE);return;}active->attribute[i].enabled=1;}void glDisableVertexAttribArray(GLuint i){if(!active||i>=LR_GL_MAX_ATTRIBUTES){gl_error(GL_INVALID_VALUE);return;}active->attribute[i].enabled=0;}
void glVertexAttrib4f(GLuint i,GLfloat x,GLfloat y,GLfloat z,GLfloat w){if(!active||i>=LR_GL_MAX_ATTRIBUTES){gl_error(GL_INVALID_VALUE);return;}float v[]={x,y,z,w};active->attribute[i].value_kind=0;memcpy(active->attribute[i].value,v,16);}
void glVertexAttrib3f(GLuint i,GLfloat x,GLfloat y,GLfloat z){glVertexAttrib4f(i,x,y,z,1);}void glVertexAttrib2f(GLuint i,GLfloat x,GLfloat y){glVertexAttrib4f(i,x,y,0,1);}void glVertexAttrib1f(GLuint i,GLfloat x){glVertexAttrib4f(i,x,0,0,1);}
void glBufferSubData(GLenum target,GLintptr offset,GLsizeiptr size,const void *data){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(target!=GL_ARRAY_BUFFER&&target!=GL_ELEMENT_ARRAY_BUFFER&&!(active->desktop&&(target==GL_UNIFORM_BUFFER||target==GL_SHADER_STORAGE_BUFFER))){gl_error(GL_INVALID_ENUM);return;}unsigned b=target==GL_ARRAY_BUFFER?active->bound_buffer:target==GL_ELEMENT_ARRAY_BUFFER?active->bound_element:target==GL_SHADER_STORAGE_BUFFER?active->bound_storage:active->bound_uniform;if(!b){gl_error(GL_INVALID_OPERATION);return;}if(active->buffers[b-1].mapped){gl_error(GL_INVALID_OPERATION);return;}if(offset<0||size<0||(size_t)offset>active->buffers[b-1].size||(size_t)size>active->buffers[b-1].size-offset||(!data&&size)){gl_error(GL_INVALID_VALUE);return;}if(size){memcpy(active->buffers[b-1].data+offset,data,size);buffer_changed(b-1);}}
void glGetIntegerv(GLenum property,GLint *out){if(!active||!out){gl_error(GL_INVALID_OPERATION);return;}switch(property){case GL_DEPTH_TEST:*out=active->depth;break;case 0x0b74:*out=active->depth_state.function;break;case 0x0b72:*out=active->depth_state.write_mask;break;case 0x0b73:*out=(GLint)round(active->depth_state.clear_depth*2147483647.0);break;case 0x90d3:*out=active->bound_storage?active->buffers[active->bound_storage-1].name:0;break;case 0x90dd:*out=LR_GL_MAX_BLOCK_BINDINGS;break;case 0x90de:*out=LR_GL_MAX_STORAGE_BYTES;break;case 0x90df:*out=16;break;case 0x90d6:case 0x90da:*out=LR_GL_MAX_STORAGE_BLOCKS;break;case 0x90dc:*out=LR_GL_MAX_STORAGE_BLOCKS*2;break;case GL_BLEND_SRC_RGB:*out=active->blend.src_rgb;break;case GL_BLEND_DST_RGB:*out=active->blend.dst_rgb;break;case GL_BLEND_SRC_ALPHA:*out=active->blend.src_alpha;break;case GL_BLEND_DST_ALPHA:*out=active->blend.dst_alpha;break;case GL_BLEND_EQUATION_RGB:*out=active->blend.equation_rgb;break;case GL_BLEND_EQUATION_ALPHA:*out=active->blend.equation_alpha;break;case 0x8a28:*out=active->bound_uniform?active->buffers[active->bound_uniform-1].name:0;break;case 0x8a2f:*out=LR_GL_MAX_BLOCK_BINDINGS;break;case 0x8a30:*out=LR_GL_MAX_BLOCK_BYTES;break;case 0x8a34:*out=16;break;case 0x8a2b:case 0x8a2d:*out=LR_GL_MAX_BLOCKS;break;case 0x8a2e:*out=LR_GL_MAX_BLOCKS*2;break;case 0x85b5:if(!active->desktop){gl_error(GL_INVALID_ENUM);return;}*out=active->vertex_array;break;case GL_FRAMEBUFFER_BINDING:*out=active->framebuffer;break;case GL_RENDERBUFFER_BINDING:*out=active->renderbuffer;break;case GL_MAX_RENDERBUFFER_SIZE:*out=LR_GPU_MAX_DIMENSION;break;case GL_MAX_TEXTURE_IMAGE_UNITS:case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:*out=LR_GPU_TEXTURE_UNITS;break;case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:*out=LR_GPU_TEXTURE_UNITS;break;case GL_MAX_TEXTURE_SIZE:*out=LR_GPU_MAX_DIMENSION;break;case GL_ACTIVE_TEXTURE:*out=GL_TEXTURE0+active->texture_unit;break;case GL_TEXTURE_BINDING_2D:*out=active->bound_texture[active->texture_unit];break;case GL_PACK_ALIGNMENT:*out=active->pack_alignment;break;case GL_VIEWPORT:out[0]=active->viewport.x;out[1]=active->viewport.y;out[2]=active->viewport.width;out[3]=active->viewport.height;break;case GL_MAX_VIEWPORT_DIMS:out[0]=out[1]=LR_GPU_MAX_DIMENSION;break;case GL_UNPACK_ALIGNMENT:*out=active->unpack_alignment;break;case GL_MAX_VERTEX_ATTRIBS:*out=LR_GL_MAX_ATTRIBUTES;break;case GL_CURRENT_PROGRAM:*out=active->active_program?active->program[active->active_program-1].name:0;break;case GL_ARRAY_BUFFER_BINDING:*out=active->bound_buffer?active->buffers[active->bound_buffer-1].name:0;break;case GL_ELEMENT_ARRAY_BUFFER_BINDING:*out=active->bound_element?active->buffers[active->bound_element-1].name:0;break;default:gl_error(GL_INVALID_ENUM);}}
void glVertexAttribDivisor(GLuint index,GLuint divisor){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(index>=LR_GL_MAX_ATTRIBUTES){gl_error(GL_INVALID_VALUE);return;}active->attribute[index].divisor=divisor;}
void glGetVertexAttribiv(GLuint index,GLenum property,GLint *out){if(!active||index>=LR_GL_MAX_ATTRIBUTES||!out){gl_error(GL_INVALID_VALUE);return;}struct lr_vertex_attribute *a=&active->attribute[index];switch(property){case 0x88fd:*out=a->integer;break;case 0x88fe:*out=a->divisor;break;case GL_VERTEX_ATTRIB_ARRAY_ENABLED:*out=a->enabled;break;case GL_VERTEX_ATTRIB_ARRAY_SIZE:*out=a->size;break;case GL_VERTEX_ATTRIB_ARRAY_STRIDE:*out=a->stride;break;case GL_VERTEX_ATTRIB_ARRAY_TYPE:*out=a->type;break;case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:*out=a->normalized;break;case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:*out=a->buffer?active->buffers[a->buffer-1].name:0;break;default:gl_error(GL_INVALID_ENUM);}}
static int attribute_active(unsigned a){const struct lr_gl_link *link=&active->program[active->active_program-1].executable;for(unsigned i=0;i<link->attributes;i++)if(link->attribute[i].location==a)return 1;return 0;}
static unsigned attribute_kind(unsigned a){
 const struct lr_gl_link *link=&active->program[active->active_program-1].executable;
 for(unsigned i=0;i<link->attributes;i++)if(link->attribute[i].location==a)return lr_gl_type_kind(link->attribute[i].type);return 0;
}
static int attribute_value(unsigned a,unsigned index,float *v){
 struct lr_vertex_attribute *attr=&active->attribute[a];unsigned kind=attribute_kind(a);
 memcpy(v,attr->value,16);if(!attr->enabled){if(kind&&kind<=2&&!attr->value_kind)for(unsigned i=0;i<4;i++){if(!isfinite(attr->value[i])||attr->value[i]<INT32_MIN||attr->value[i]>=2147483648.0f){gl_error(GL_INVALID_OPERATION);return 0;}int32_t value=(int32_t)attr->value[i];memcpy(v+i,&value,4);}return 1;}
 if((kind==1||kind==2)!=!!attr->integer){gl_error(GL_INVALID_OPERATION);return 0;}
 unsigned element=(attr->type==GL_FLOAT||attr->type==0x1404||attr->type==0x1405)?4:(attr->type==GL_SHORT||attr->type==GL_UNSIGNED_SHORT)?2:1;
 const char *p=array_pointer(attr->pointer,attr->buffer,(size_t)index*attr->stride,attr->size*element);if(!p){gl_error(GL_INVALID_OPERATION);return 0;}
 memset(v,0,16);if(attr->integer){uint32_t one=1;memcpy(v+3,&one,4);}else v[3]=1;
 for(int c=0;c<attr->size;c++){
  if(attr->integer){uint32_t value;if(attr->type==GL_BYTE){int8_t z;memcpy(&z,p+c,1);value=(int32_t)z;}else if(attr->type==GL_UNSIGNED_BYTE){uint8_t z;memcpy(&z,p+c,1);value=z;}else if(attr->type==GL_SHORT){int16_t z;memcpy(&z,p+c*2,2);value=(int32_t)z;}else if(attr->type==GL_UNSIGNED_SHORT){uint16_t z;memcpy(&z,p+c*2,2);value=z;}else memcpy(&value,p+c*4,4);memcpy(v+c,&value,4);}
  else if(element==4)memcpy(v+c,p+c*4,4);
  else {float x,max;if(attr->type==GL_BYTE){int8_t z;memcpy(&z,p+c,1);x=z;max=127;}else if(attr->type==GL_UNSIGNED_BYTE){uint8_t z;memcpy(&z,p+c,1);x=z;max=255;}else if(attr->type==GL_SHORT){int16_t z;memcpy(&z,p+c*2,2);x=z;max=32767;}else{uint16_t z;memcpy(&z,p+c*2,2);x=z;max=65535;}v[c]=attr->normalized?fmaxf(-1,x/max):x;}
 }return 1;
}
static int instance_attributes(unsigned instances){
 unsigned used=0;for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)if(attribute_active(a)&&active->attribute[a].enabled&&active->attribute[a].divisor)used++;
 if(!used&&!active->instance_live)return 1;
 unsigned base=active->base_instance;
 // Metal fetches per-instance attributes with the native base index. Expand
 // divisor data and retain the prefix so GL fetches base + floor(i/divisor).
 if(used&&base&&base>LR_GPU_MAX_INSTANCES-instances){gl_error(GL_OUT_OF_MEMORY);return 0;}
 struct lr_gpu_instance_attributes packet={.instances=used&&base?base+instances:instances};
 for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)if(attribute_active(a)&&active->attribute[a].enabled&&active->attribute[a].divisor){unsigned divisor=active->attribute[a].divisor;
  if(base){packet.divisor[a]=1;packet.count[a]=base+instances;for(unsigned i=0;i<instances;i++)if(!attribute_value(a,base+i/divisor,packet.value[a][base+i]))return 0;}
  else{packet.divisor[a]=divisor;packet.count[a]=((uint64_t)instances+divisor-1)/divisor;for(unsigned i=0;i<packet.count[a];i++)if(!attribute_value(a,i,packet.value[a][i]))return 0;}
 }
 struct lr_gpu_request r={.operation=LR_GPU_INSTANCE_ATTRIBUTES,.flags=LR_GPU_TYPED_DATA_BIT,.address=(uintptr_t)&packet,.bytes=sizeof packet};if(!submit(&r))return 0;active->instance_live=used;return 1;
}
static void draw_program_instances(GLenum mode,GLint first,GLsizei count,GLenum type,const void *indices,unsigned instances){
 if(!active||!active->active_program||!active->program[active->active_program-1].can_draw){gl_error(GL_INVALID_OPERATION);return;}if(mode!=GL_TRIANGLES&&mode!=GL_TRIANGLE_STRIP&&mode!=GL_TRIANGLE_FAN){gl_error(GL_INVALID_ENUM);return;}if(first<0||count<0||first>LR_GPU_MAX_VERTICES||count>LR_GPU_MAX_VERTICES){gl_error(GL_INVALID_VALUE);return;}if(type&&type!=GL_UNSIGNED_BYTE&&type!=GL_UNSIGNED_SHORT&&!(active->desktop&&type==GL_UNSIGNED_INT)){gl_error(GL_INVALID_ENUM);return;}
 if(type){size_t bytes=(size_t)count*(type==GL_UNSIGNED_BYTE?1:type==GL_UNSIGNED_SHORT?2:4);indices=array_pointer(indices,active->bound_element,0,bytes);if(!indices){gl_error(GL_INVALID_OPERATION);return;}}
 if(!instances)return;
 unsigned triangles=mode==GL_TRIANGLES?count/3:(count>2?count-2:0),n=triangles*3;if(!n)return;if(n>LR_GPU_MAX_VERTICES){gl_error(GL_OUT_OF_MEMORY);return;}
 if(!active->program_scratch){active->program_scratch=malloc(sizeof(struct lr_gl_draw)+LR_GPU_MAX_VERTICES*(sizeof(struct lr_gl_vertex)+sizeof(uint32_t))+4);if(!active->program_scratch){gl_error(GL_OUT_OF_MEMORY);return;}}
 struct lr_gl_draw *packet=active->program_scratch;packet->program=active->active_program;packet->count=0;memcpy(packet->uniform,active->program[active->active_program-1].values,sizeof packet->uniform);
 for(unsigned i=0;i<active->program[active->active_program-1].executable.uniforms;i++)if(active->program[active->active_program-1].executable.uniform[i].type==GL_SAMPLER_2D){GLint unit=active->program[active->active_program-1].sampler_unit[i];if(unit<0||unit>=LR_GPU_TEXTURE_UNITS){gl_error(GL_INVALID_OPERATION);return;}packet->uniform[i][0]=active->bound_texture[unit];}

 uint32_t elementIds[LR_GPU_MAX_VERTICES];
 for(unsigned t=0;t<triangles;t++){unsigned ids[3];if(mode==GL_TRIANGLES){ids[0]=t*3;ids[1]=t*3+1;ids[2]=t*3+2;}else if(mode==GL_TRIANGLE_FAN){ids[0]=0;ids[1]=t+1;ids[2]=t+2;}else{ids[0]=t+(t&1);ids[1]=t+1-(t&1);ids[2]=t+2;}
  for(unsigned k=0;k<3;k++){unsigned index=first+ids[k];if(type==GL_UNSIGNED_BYTE)index=((const uint8_t *)indices)[ids[k]];else if(type==GL_UNSIGNED_SHORT){uint16_t v;memcpy(&v,(const char *)indices+ids[k]*2,2);index=v;}else if(type==GL_UNSIGNED_INT)memcpy(&index,(const char *)indices+ids[k]*4,4);
   if(index>=LR_GPU_MAX_VERTICES){gl_error(GL_OUT_OF_MEMORY);return;}// Metal uses the first provoking vertex; GL triangles use the last.
   // A cyclic permutation preserves winding and interpolation.
   elementIds[t*3+(k+1)%3]=index;if(index>=packet->count)packet->count=index+1;
  }
 }
 memset(packet->vertex,0,(size_t)packet->count*sizeof(struct lr_gl_vertex));uint32_t seen[LR_GPU_MAX_VERTICES/32]={0};
 for(unsigned j=0;j<n;j++){unsigned index=elementIds[j];if(seen[index/32]&(1u<<(index%32)))continue;seen[index/32]|=1u<<(index%32);
   for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)if(attribute_active(a)&&(!active->attribute[a].enabled||!active->attribute[a].divisor)&&!attribute_value(a,index,packet->vertex[index].attribute[a]))return;
 }
 if(!framebuffer_ready(NULL,NULL))return;
 if(!upload_uniform_blocks(active->active_program-1)||!upload_storage_blocks(active->active_program-1)||!instance_attributes(instances))return;
 uint32_t *outIndices=(void *)&packet->vertex[packet->count];memcpy(outIndices,elementIds,n*sizeof(uint32_t));
 struct lr_gpu_request r={.operation=active->base_instance?LR_GPU_DRAW_INDEXED_BASE:LR_GPU_DRAW_INDEXED,.width=instances,.height=n,.flags=LR_GPU_TYPED_DATA_BIT|(active->depth?LR_GPU_DEPTH_TEST_BIT:0)|(active->cull?LR_GPU_CULL_BIT:0),.count=packet->count,.address=(uintptr_t)packet,.bytes=sizeof *packet+(size_t)packet->count*sizeof(struct lr_gl_vertex)+(size_t)n*sizeof(uint32_t)};if(active->base_instance){memcpy((char *)packet+r.bytes,&active->base_instance,4);r.bytes+=4;}submit(&r);
}

static void draw_program(GLenum mode,GLint first,GLsizei count,GLenum type,const void *indices){draw_program_instances(mode,first,count,type,indices,1);}
void glDrawArraysInstanced(GLenum mode,GLint first,GLsizei count,GLsizei instances){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(instances<0){gl_error(GL_INVALID_VALUE);return;}if(instances>LR_GPU_MAX_INSTANCES){gl_error(GL_OUT_OF_MEMORY);return;}draw_program_instances(mode,first,count,0,NULL,instances);}
void glDrawElementsInstanced(GLenum mode,GLsizei count,GLenum type,const void *indices,GLsizei instances){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(instances<0){gl_error(GL_INVALID_VALUE);return;}if(instances>LR_GPU_MAX_INSTANCES){gl_error(GL_OUT_OF_MEMORY);return;}draw_program_instances(mode,0,count,type,indices,instances);}

void glDrawArraysInstancedBaseInstance(GLenum mode,GLint first,GLsizei count,GLsizei instances,GLuint base){
 if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(instances<0||base>UINT32_MAX-(unsigned)instances){gl_error(GL_INVALID_VALUE);return;}if(instances>LR_GPU_MAX_INSTANCES){gl_error(GL_OUT_OF_MEMORY);return;}
 active->base_instance=base;draw_program_instances(mode,first,count,0,NULL,instances);active->base_instance=0;
}
void glDrawElementsInstancedBaseInstance(GLenum mode,GLsizei count,GLenum type,const void *indices,GLsizei instances,GLuint base){
 if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(instances<0||base>UINT32_MAX-(unsigned)instances){gl_error(GL_INVALID_VALUE);return;}if(instances>LR_GPU_MAX_INSTANCES){gl_error(GL_OUT_OF_MEMORY);return;}
 active->base_instance=base;draw_program_instances(mode,0,count,type,indices,instances);active->base_instance=0;
}
