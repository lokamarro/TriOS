// Bounded GLES2 texture objects. System Mesa is never replaced globally.
static void texture_defaults(unsigned name){
 if(!active->textures[name].min_filter){active->textures[name].min_filter=GL_NEAREST_MIPMAP_LINEAR;active->textures[name].mag_filter=GL_LINEAR;active->textures[name].wrap_s=active->textures[name].wrap_t=GL_REPEAT;}
}
static size_t texture_storage(unsigned w,unsigned h){size_t n=0;if(!w||!h)return 0;for(;;){n+=(size_t)w*h*4;if(w==1&&h==1)return n;w=w>1?w/2:1;h=h>1?h/2:1;}}
static int texture_api(GLenum target){if(!active||(active->client_version!=2&&!active->desktop)){gl_error(GL_INVALID_OPERATION);return 0;}if(target!=GL_TEXTURE_2D){gl_error(GL_INVALID_ENUM);return 0;}return 1;}
void glActiveTexture(GLenum unit){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(unit<GL_TEXTURE0||unit>=GL_TEXTURE0+LR_GPU_TEXTURE_UNITS){gl_error(GL_INVALID_ENUM);return;}active->texture_unit=unit-GL_TEXTURE0;}
void glGenTextures(GLsizei n,GLuint *names){
 if(!active){gl_error(GL_INVALID_OPERATION);return;}if(n<0||(!names&&n)){gl_error(GL_INVALID_VALUE);return;}if(n>LR_GPU_MAX_TEXTURES){gl_error(GL_OUT_OF_MEMORY);return;}
 for(int k=0;k<n;k++){names[k]=0;for(unsigned i=1;i<=LR_GPU_MAX_TEXTURES;i++)if(!active->textures[i].reserved){active->textures[i].reserved=1;texture_defaults(i);names[k]=i;break;}if(!names[k]){gl_error(GL_OUT_OF_MEMORY);break;}}
}
void glBindTexture(GLenum target,GLuint name){if(!texture_api(target))return;if(name>LR_GPU_MAX_TEXTURES){gl_error(GL_INVALID_VALUE);return;}texture_defaults(name);active->textures[name].reserved=active->textures[name].bound=1;active->bound_texture[active->texture_unit]=name;}
GLboolean glIsTexture(GLuint name){return active&&name&&name<=LR_GPU_MAX_TEXTURES&&active->textures[name].bound?GL_TRUE:GL_FALSE;}
void glDeleteTextures(GLsizei n,const GLuint *names){
 if(!active){gl_error(GL_INVALID_OPERATION);return;}if(n<0||(!names&&n)){gl_error(GL_INVALID_VALUE);return;}
 for(int k=0;k<n;k++){unsigned name=names[k];if(!name||name>LR_GPU_MAX_TEXTURES||!active->textures[name].reserved)continue;
  struct lr_gpu_request r={.operation=LR_GPU_DELETE_TEXTURE,.count=name};if(!submit(&r))return;
  active->texture_bytes-=texture_storage(active->textures[name].width,active->textures[name].height);memset(&active->textures[name],0,sizeof active->textures[name]);
  for(unsigned u=0;u<LR_GPU_TEXTURE_UNITS;u++)if(active->bound_texture[u]==name)active->bound_texture[u]=0;
 }
}
void glPixelStorei(GLenum property,GLint value){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(property!=GL_UNPACK_ALIGNMENT&&property!=GL_PACK_ALIGNMENT){gl_error(GL_INVALID_ENUM);return;}if(value!=1&&value!=2&&value!=4&&value!=8){gl_error(GL_INVALID_VALUE);return;}if(property==GL_UNPACK_ALIGNMENT)active->unpack_alignment=value;else active->pack_alignment=value;}
static int texture_send(unsigned name,unsigned w,unsigned h,unsigned x,unsigned y,unsigned flags,const void *data,unsigned format){
 unsigned components=format==GL_RGBA?4:format==GL_RGB?3:(format==GL_LUMINANCE_ALPHA||format==0x8227)?2:1;
 size_t bytes=(flags==LR_GPU_TEXTURE_STATE||flags==LR_GPU_TEXTURE_STORAGE_CLEAR)?0:(size_t)w*h*4;
 struct lr_gpu_texture *packet=malloc(sizeof *packet+bytes);if(!packet){gl_error(GL_OUT_OF_MEMORY);return 0;}
 *packet=(struct lr_gpu_texture){name,w,h,x,y,active->textures[name].min_filter,active->textures[name].mag_filter,active->textures[name].wrap_s,active->textures[name].wrap_t};
 if(bytes){
  if(!data)memset(packet->pixels,0,bytes);
  else{size_t stride=((size_t)w*components+active->unpack_alignment-1)&~(size_t)(active->unpack_alignment-1);
   if(format==GL_RGBA){
    if(stride==(size_t)w*4)memcpy(packet->pixels,data,bytes);
    else for(unsigned row=0;row<h;row++)memcpy(packet->pixels+(size_t)row*w*4,(const char*)data+row*stride,(size_t)w*4);
   }else for(unsigned row=0;row<h;row++)for(unsigned col=0;col<w;col++){
    const unsigned char *src=(const unsigned char*)data+row*stride+col*components;unsigned char *dst=packet->pixels+((size_t)row*w+col)*4;
    if(format==GL_RGBA)memcpy(dst,src,4);
    else if(format==GL_RGB){memcpy(dst,src,3);dst[3]=255;}
    else if(format==0x1903||format==0x8227){dst[0]=src[0];dst[1]=format==0x8227?src[1]:0;dst[2]=0;dst[3]=255;}
    else if(format==GL_ALPHA){dst[0]=dst[1]=dst[2]=0;dst[3]=src[0];}
    else {dst[0]=dst[1]=dst[2]=src[0];dst[3]=format==GL_LUMINANCE_ALPHA?src[1]:255;}
   }
  }
 }
 struct lr_gpu_request r={.operation=LR_GPU_TEXTURE,.count=format,.width=w,.height=h,.flags=flags,.address=(uintptr_t)packet,.bytes=sizeof *packet+bytes};int ok=submit(&r);free(packet);return ok;
}
static int texture_format(GLenum format,GLenum type){if(type!=GL_UNSIGNED_BYTE||!(format==GL_RGBA||format==GL_RGB||format==GL_ALPHA||format==GL_LUMINANCE||format==GL_LUMINANCE_ALPHA||(active->desktop&&(format==0x1903||format==0x8227)))){gl_error(GL_INVALID_ENUM);return 0;}return 1;}
void glTexImage2D(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const void *data){
 if(!texture_api(target))return;if(level<0||w<0||h<0||w>LR_GPU_MAX_DIMENSION||h>LR_GPU_MAX_DIMENSION||border){gl_error(GL_INVALID_VALUE);return;}
 if(level){gl_error(GL_INVALID_OPERATION);return;}if(!texture_format(format,type))return;if((unsigned)internal!=format&&!(active->desktop&&((internal==0x8229&&format==0x1903)||(internal==0x822b&&format==0x8227)||(internal==0x8058&&format==GL_RGBA)||(internal==0x8051&&format==GL_RGB)))){gl_error(GL_INVALID_OPERATION);return;}
 unsigned name=active->bound_texture[active->texture_unit];texture_defaults(name);
 size_t old=texture_storage(active->textures[name].width,active->textures[name].height),bytes=texture_storage(w,h);
 if(bytes>LR_GPU_TEXTURE_BUDGET-(active->texture_bytes-old)){gl_error(GL_OUT_OF_MEMORY);return;}
 if(!w||!h){if(!texture_send(name,0,0,0,0,LR_GPU_TEXTURE_STORAGE_CLEAR,NULL,format))return;}
 else if(!texture_send(name,w,h,0,0,0,data,format))return;
 active->texture_bytes=active->texture_bytes-old+bytes;active->textures[name].width=w;active->textures[name].height=h;active->textures[name].format=format;
}
void glTexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,const void *data){
 if(!texture_api(target))return;if(level<0||x<0||y<0||w<0||h<0){gl_error(GL_INVALID_VALUE);return;}if(level){gl_error(GL_INVALID_OPERATION);return;}if(!texture_format(format,type))return;
 unsigned name=active->bound_texture[active->texture_unit];unsigned width=active->textures[name].width,height=active->textures[name].height;
 if(!width||!height||(unsigned)x>width||(unsigned)y>height||(unsigned)w>width-x||(unsigned)h>height-y){gl_error(GL_INVALID_VALUE);return;}
 if(active->textures[name].format!=format){gl_error(GL_INVALID_OPERATION);return;}if(!w||!h)return;if(!data){gl_error(GL_INVALID_VALUE);return;}
 texture_send(name,w,h,x,y,LR_GPU_TEXTURE_SUBIMAGE,data,format);
}
void glTexParameteri(GLenum target,GLenum property,GLint value){
 if(!texture_api(target))return;unsigned name=active->bound_texture[active->texture_unit];texture_defaults(name);unsigned *slot;
 switch(property){case GL_TEXTURE_MIN_FILTER:if(!(value==GL_NEAREST||value==GL_LINEAR||(value>=GL_NEAREST_MIPMAP_NEAREST&&value<=GL_LINEAR_MIPMAP_LINEAR))){gl_error(GL_INVALID_ENUM);return;}slot=&active->textures[name].min_filter;break;
 case GL_TEXTURE_MAG_FILTER:if(value!=GL_NEAREST&&value!=GL_LINEAR){gl_error(GL_INVALID_ENUM);return;}slot=&active->textures[name].mag_filter;break;
 case GL_TEXTURE_WRAP_S:case GL_TEXTURE_WRAP_T:if(value!=GL_CLAMP_TO_EDGE&&value!=GL_REPEAT&&value!=GL_MIRRORED_REPEAT){gl_error(GL_INVALID_ENUM);return;}slot=property==GL_TEXTURE_WRAP_S?&active->textures[name].wrap_s:&active->textures[name].wrap_t;break;
 default:gl_error(GL_INVALID_ENUM);return;}
 unsigned old=*slot;*slot=value;if(active->textures[name].width&&active->textures[name].height&&!texture_send(name,active->textures[name].width,active->textures[name].height,0,0,LR_GPU_TEXTURE_STATE,NULL,GL_RGBA))*slot=old;
}
void glTexParameterf(GLenum target,GLenum property,GLfloat value){if(!isfinite(value)||value<(float)INT32_MIN||value>=(float)INT32_MAX){gl_error(GL_INVALID_VALUE);return;}glTexParameteri(target,property,(GLint)value);}
void glGetTexParameteriv(GLenum target,GLenum property,GLint *out){if(!texture_api(target))return;if(!out){gl_error(GL_INVALID_VALUE);return;}unsigned name=active->bound_texture[active->texture_unit];texture_defaults(name);switch(property){case GL_TEXTURE_MIN_FILTER:*out=active->textures[name].min_filter;break;case GL_TEXTURE_MAG_FILTER:*out=active->textures[name].mag_filter;break;case GL_TEXTURE_WRAP_S:*out=active->textures[name].wrap_s;break;case GL_TEXTURE_WRAP_T:*out=active->textures[name].wrap_t;break;default:gl_error(GL_INVALID_ENUM);}}
void glGenerateMipmap(GLenum target){if(!texture_api(target))return;unsigned name=active->bound_texture[active->texture_unit],w=active->textures[name].width,h=active->textures[name].height;if(!w||!h||(w&(w-1))||(h&(h-1))){gl_error(GL_INVALID_OPERATION);return;}struct lr_gpu_request r={.operation=LR_GPU_GENERATE_MIPMAP,.count=name};submit(&r);}
void glUniform1iv(GLint location,GLsizei count,const GLint *values){uniform_integer(location,count,values,1,0);}
void glUniform1i(GLint location,GLint value){glUniform1iv(location,1,&value);}
void glGetUniformiv(GLuint name,GLint location,GLint *out){uniform_query(name,location,out,1);}
