// Typed default uniforms and vertex data preserve integer bits through the ABI.
static void uniform_integer(GLint location,GLsizei count,const void *values,unsigned components,unsigned unsign){
 if(location==-1)return;if(!active||!active->active_program){gl_error(GL_INVALID_OPERATION);return;}unsigned p=active->active_program-1;
 unsigned first=0;int slot=uniform_slot(p,location,&first);if(slot<0){gl_error(GL_INVALID_OPERATION);return;}
 unsigned type=active->program[p].executable.uniform[slot].type,kind=lr_gl_type_kind(type);
 if(lr_gl_type_components(type)!=components||!(kind==3||(unsign?kind==2:kind==1||kind==4))||(unsign&&!active->desktop)){gl_error(GL_INVALID_OPERATION);return;}
 if(active->program[p].extents.uniform[slot].elements>1){uniform_array_update(p,slot,first,count,values);return;}
 if(count<0||(!values&&count)){gl_error(GL_INVALID_VALUE);return;}if(count>1){gl_error(GL_INVALID_OPERATION);return;}if(!count)return;location=slot;
 if(kind==4){GLint unit;memcpy(&unit,values,4);active->program[p].sampler_unit[location]=unit;active->program[p].values[location][0]=(float)unit;}
 else if(kind==3)for(unsigned i=0;i<components;i++){uint32_t value;memcpy(&value,(const char *)values+i*4,4);value=!!value;memcpy(&active->program[p].values[location][i],&value,4);}
 else memcpy(active->program[p].values[location],values,components*4);
}
static void uniform_query(GLuint program,GLint location,void *out,unsigned output){
 int p=program_index(program);if(p<0)return;if(!out){gl_error(GL_INVALID_VALUE);return;}
 unsigned first=0;int slot=uniform_slot(p,location,&first);if(!active->program[p].link.status||slot<0){gl_error(GL_INVALID_OPERATION);return;}
 unsigned type=active->program[p].link.uniform[slot].type,kind=lr_gl_type_kind(type),n=lr_gl_type_components(type);
 float zero[16]={0};const float *source=active->program[p].extents.uniform[slot].elements>1?(active->program[p].array_values[slot]?(const float *)(active->program[p].array_values[slot]+(size_t)first*n*4):zero):active->program[p].values[slot];
 for(unsigned i=0;i<n;i++){uint32_t raw;memcpy(&raw,source+i,4);double value=kind==1?(double)(int32_t)raw:kind==2||kind==3?(double)raw:source[i];
  if(!output){float v=value;memcpy((char *)out+i*4,&v,4);}
  else if(output==1){int32_t v=value>=INT32_MAX?INT32_MAX:value<=INT32_MIN?INT32_MIN:isnan(value)?0:(int32_t)round(value);memcpy((char *)out+i*4,&v,4);}
  else {uint32_t v=value>=UINT32_MAX?UINT32_MAX:value<=0||isnan(value)?0:(uint32_t)round(value);memcpy((char *)out+i*4,&v,4);}
 }
}
#define LR_INT_UNIFORM(N) \
void glUniform##N##iv(GLint l,GLsizei n,const GLint *v){uniform_integer(l,n,v,N,0);}
LR_INT_UNIFORM(2) LR_INT_UNIFORM(3) LR_INT_UNIFORM(4)
#define LR_UINT_UNIFORM(N) \
void glUniform##N##uiv(GLint l,GLsizei n,const GLuint *v){uniform_integer(l,n,v,N,1);}
LR_UINT_UNIFORM(1) LR_UINT_UNIFORM(2) LR_UINT_UNIFORM(3) LR_UINT_UNIFORM(4)
void glUniform2i(GLint l,GLint x,GLint y){GLint v[]={x,y};glUniform2iv(l,1,v);}void glUniform3i(GLint l,GLint x,GLint y,GLint z){GLint v[]={x,y,z};glUniform3iv(l,1,v);}void glUniform4i(GLint l,GLint x,GLint y,GLint z,GLint w){GLint v[]={x,y,z,w};glUniform4iv(l,1,v);}
void glUniform1ui(GLint l,GLuint x){glUniform1uiv(l,1,&x);}void glUniform2ui(GLint l,GLuint x,GLuint y){GLuint v[]={x,y};glUniform2uiv(l,1,v);}void glUniform3ui(GLint l,GLuint x,GLuint y,GLuint z){GLuint v[]={x,y,z};glUniform3uiv(l,1,v);}void glUniform4ui(GLint l,GLuint x,GLuint y,GLuint z,GLuint w){GLuint v[]={x,y,z,w};glUniform4uiv(l,1,v);}
void glGetUniformuiv(GLuint p,GLint l,GLuint *v){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}uniform_query(p,l,v,2);}
void glVertexAttribIPointer(GLuint index,GLint size,GLenum type,GLsizei stride,const void *pointer){
 if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(index>=LR_GL_MAX_ATTRIBUTES||size<1||size>4||stride<0){gl_error(GL_INVALID_VALUE);return;}
 unsigned bytes=type==0x1404||type==0x1405?4:type==GL_SHORT||type==GL_UNSIGNED_SHORT?2:type==GL_BYTE||type==GL_UNSIGNED_BYTE?1:0;if(!bytes){gl_error(GL_INVALID_ENUM);return;}
 struct lr_vertex_attribute *a=&active->attribute[index];a->integer=1;a->size=size;a->type=type;a->normalized=0;a->stride=stride?stride:size*bytes;a->buffer=active->bound_buffer;a->pointer=pointer;
}
static void integer_attribute(GLuint index,const void *v,unsigned kind){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(index>=LR_GL_MAX_ATTRIBUTES||!v){gl_error(GL_INVALID_VALUE);return;}active->attribute[index].value_kind=kind;memcpy(active->attribute[index].value,v,16);}
void glVertexAttribI4i(GLuint i,GLint x,GLint y,GLint z,GLint w){GLint v[]={x,y,z,w};integer_attribute(i,v,1);}void glVertexAttribI4ui(GLuint i,GLuint x,GLuint y,GLuint z,GLuint w){GLuint v[]={x,y,z,w};integer_attribute(i,v,2);}void glVertexAttribI4iv(GLuint i,const GLint *v){integer_attribute(i,v,1);}void glVertexAttribI4uiv(GLuint i,const GLuint *v){integer_attribute(i,v,2);}
static void integer_attribute_query(GLuint i,GLenum property,void *out,unsigned unsign){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(i>=LR_GL_MAX_ATTRIBUTES||!out){gl_error(GL_INVALID_VALUE);return;}if(property==GL_CURRENT_VERTEX_ATTRIB){struct lr_vertex_attribute *a=&active->attribute[i];if(a->value_kind)memcpy(out,a->value,16);else for(unsigned k=0;k<4;k++){double f=a->value[k];uint32_t value=unsign?(f>=UINT32_MAX?UINT32_MAX:f<=0||isnan(f)?0:(uint32_t)f):(uint32_t)(f>=INT32_MAX?INT32_MAX:f<=INT32_MIN?INT32_MIN:isnan(f)?0:(int32_t)f);memcpy((char *)out+k*4,&value,4);}}else glGetVertexAttribiv(i,property,out);}
void glGetVertexAttribIiv(GLuint i,GLenum p,GLint *v){integer_attribute_query(i,p,v,0);}void glGetVertexAttribIuiv(GLuint i,GLenum p,GLuint *v){integer_attribute_query(i,p,v,1);}
