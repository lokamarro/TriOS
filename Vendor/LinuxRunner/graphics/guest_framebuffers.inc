// RGBA color-texture FBOs and bounded depth renderbuffers. Stencil/MRT/MSAA
// and depth textures remain unsupported. The host owns attachment references even after name deletion.
static int framebuffer_api(GLenum target){
 if(!active||(active->client_version!=2&&!active->desktop)){gl_error(GL_INVALID_OPERATION);return 0;}
 if(target!=GL_FRAMEBUFFER){gl_error(GL_INVALID_ENUM);return 0;}return 1;
}
static int framebuffer_command(struct lr_gpu_framebuffer *p){
 struct lr_gpu_request r={.operation=LR_GPU_FRAMEBUFFER,.address=(uintptr_t)p,.bytes=sizeof *p};return submit(&r);
}
static int framebuffer_ready(unsigned *width,unsigned *height){
 if(!active->framebuffer)return 1;
 struct lr_gpu_framebuffer p={.command=LR_GPU_FB_QUERY};if(!framebuffer_command(&p))return 0;
 if(p.status!=GL_FRAMEBUFFER_COMPLETE){gl_error(GL_INVALID_FRAMEBUFFER_OPERATION);return 0;}
 if(width)*width=p.width;if(height)*height=p.height;return 1;
}
void glGenFramebuffers(GLsizei n,GLuint *names){
 if(!framebuffer_api(GL_FRAMEBUFFER))return;if(n<0||(!names&&n)){gl_error(GL_INVALID_VALUE);return;}
 if(n>LR_GPU_MAX_FRAMEBUFFERS){gl_error(GL_OUT_OF_MEMORY);return;}
 for(int k=0;k<n;k++){names[k]=0;for(unsigned i=1;i<=LR_GPU_MAX_FRAMEBUFFERS;i++)if(!active->framebuffers[i].reserved){active->framebuffers[i].reserved=1;names[k]=i;break;}if(!names[k]){gl_error(GL_OUT_OF_MEMORY);break;}}
}
void glBindFramebuffer(GLenum target,GLuint name){
 if(!framebuffer_api(target))return;if(name>LR_GPU_MAX_FRAMEBUFFERS){gl_error(GL_INVALID_VALUE);return;}
 struct lr_gpu_framebuffer p={.command=LR_GPU_FB_BIND,.name=name};
 if(framebuffer_command(&p)){active->framebuffer=name;if(name)active->framebuffers[name].reserved=active->framebuffers[name].bound=1;}
}
GLboolean glIsFramebuffer(GLuint name){return active&&name&&name<=LR_GPU_MAX_FRAMEBUFFERS&&active->framebuffers[name].bound?GL_TRUE:GL_FALSE;}
void glDeleteFramebuffers(GLsizei n,const GLuint *names){
 if(!framebuffer_api(GL_FRAMEBUFFER))return;if(n<0||(!names&&n)){gl_error(GL_INVALID_VALUE);return;}
 for(int k=0;k<n;k++){unsigned name=names[k];if(!name||name>LR_GPU_MAX_FRAMEBUFFERS||!active->framebuffers[name].reserved)continue;
  struct lr_gpu_framebuffer p={.command=LR_GPU_FB_DELETE,.name=name};if(!framebuffer_command(&p))return;
  if(active->framebuffer==name)active->framebuffer=0;memset(&active->framebuffers[name],0,sizeof active->framebuffers[name]);
 }
}
void glFramebufferTexture2D(GLenum target,GLenum attachment,GLenum textureTarget,GLuint texture,GLint level){
 if(!framebuffer_api(target))return;if(attachment!=GL_COLOR_ATTACHMENT0||textureTarget!=GL_TEXTURE_2D){gl_error(GL_INVALID_ENUM);return;}
 if(level){gl_error(GL_INVALID_VALUE);return;}if(!active->framebuffer||(texture&&!glIsTexture(texture))){gl_error(GL_INVALID_OPERATION);return;}
 struct lr_gpu_framebuffer p={.command=LR_GPU_FB_ATTACH,.name=active->framebuffer,.texture=texture,.format=GL_RGBA};framebuffer_command(&p);
}
GLenum glCheckFramebufferStatus(GLenum target){
 if(!framebuffer_api(target))return 0;struct lr_gpu_framebuffer p={.command=LR_GPU_FB_QUERY};return framebuffer_command(&p)?p.status:0;
}
void glGetFramebufferAttachmentParameteriv(GLenum target,GLenum attachment,GLenum property,GLint *out){
 if(!framebuffer_api(target))return;if(attachment!=GL_COLOR_ATTACHMENT0&&attachment!=GL_DEPTH_ATTACHMENT){gl_error(GL_INVALID_ENUM);return;}
 if(!out){gl_error(GL_INVALID_VALUE);return;}if(!active->framebuffer){gl_error(GL_INVALID_OPERATION);return;}
 struct lr_gpu_framebuffer p={.command=LR_GPU_FB_QUERY,.format=attachment==GL_DEPTH_ATTACHMENT?GL_DEPTH_ATTACHMENT:0};if(!framebuffer_command(&p))return;
 if(attachment==GL_DEPTH_ATTACHMENT){switch(property){case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:*out=p.texture?GL_RENDERBUFFER:GL_NONE;break;case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:if(!p.texture){gl_error(GL_INVALID_ENUM);return;}*out=p.texture;break;default:gl_error(GL_INVALID_ENUM);}return;}
 switch(property){case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:*out=p.texture?GL_TEXTURE:GL_NONE;break;
 case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:if(!p.texture){gl_error(GL_INVALID_ENUM);return;}*out=p.texture;break;
 case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:if(!p.texture){gl_error(GL_INVALID_ENUM);return;}*out=0;break;
 case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:if(!p.texture){gl_error(GL_INVALID_ENUM);return;}*out=0;break;
 default:gl_error(GL_INVALID_ENUM);}
}

static int renderbuffer_api(GLenum target){if(!active||(active->client_version!=2&&!active->desktop)){gl_error(GL_INVALID_OPERATION);return 0;}if(target!=GL_RENDERBUFFER){gl_error(GL_INVALID_ENUM);return 0;}return 1;}
static int renderbuffer_command(struct lr_gpu_renderbuffer *p){struct lr_gpu_request r={.operation=LR_GPU_RENDERBUFFER,.address=(uintptr_t)p,.bytes=sizeof(*p)};return submit(&r);}
void glGenRenderbuffers(GLsizei n,GLuint *names){if(!renderbuffer_api(GL_RENDERBUFFER))return;if(n<0||(!names&&n)){gl_error(GL_INVALID_VALUE);return;}if(n>LR_GPU_MAX_RENDERBUFFERS){gl_error(GL_OUT_OF_MEMORY);return;}for(int k=0;k<n;k++){names[k]=0;for(unsigned i=1;i<=LR_GPU_MAX_RENDERBUFFERS;i++)if(!active->renderbuffers[i].reserved){active->renderbuffers[i].reserved=1;names[k]=i;break;}if(!names[k]){gl_error(GL_OUT_OF_MEMORY);break;}}}
void glBindRenderbuffer(GLenum target,GLuint name){if(!renderbuffer_api(target))return;if(name>LR_GPU_MAX_RENDERBUFFERS){gl_error(GL_INVALID_VALUE);return;}if(name){struct lr_gpu_renderbuffer p={.command=LR_GPU_RB_BIND,.name=name};if(!renderbuffer_command(&p))return;active->renderbuffers[name].reserved=active->renderbuffers[name].bound=1;}active->renderbuffer=name;}
GLboolean glIsRenderbuffer(GLuint name){return active&&name&&name<=LR_GPU_MAX_RENDERBUFFERS&&active->renderbuffers[name].bound;}
void glRenderbufferStorage(GLenum target,GLenum format,GLsizei width,GLsizei height){if(!renderbuffer_api(target))return;if(format!=GL_DEPTH_COMPONENT16&&!(active->desktop&&(format==0x81a6||format==0x8cac))){gl_error(GL_INVALID_ENUM);return;}if(width<0||height<0||width>LR_GPU_MAX_DIMENSION||height>LR_GPU_MAX_DIMENSION){gl_error(GL_INVALID_VALUE);return;}if(!active->renderbuffer){gl_error(GL_INVALID_OPERATION);return;}struct lr_gpu_renderbuffer p={.command=LR_GPU_RB_STORAGE,.name=active->renderbuffer,.width=width,.height=height,.format=format};renderbuffer_command(&p);}
void glDeleteRenderbuffers(GLsizei n,const GLuint *names){if(!renderbuffer_api(GL_RENDERBUFFER))return;if(n<0||(!names&&n)){gl_error(GL_INVALID_VALUE);return;}for(int k=0;k<n;k++){unsigned name=names[k];if(!name||name>LR_GPU_MAX_RENDERBUFFERS||!active->renderbuffers[name].reserved)continue;if(active->renderbuffers[name].bound){struct lr_gpu_renderbuffer p={.command=LR_GPU_RB_DELETE,.name=name};if(!renderbuffer_command(&p))return;}if(active->renderbuffer==name)active->renderbuffer=0;memset(&active->renderbuffers[name],0,sizeof active->renderbuffers[name]);}}
void glFramebufferRenderbuffer(GLenum target,GLenum attachment,GLenum renderbufferTarget,GLuint renderbuffer){if(!framebuffer_api(target))return;if(attachment!=GL_DEPTH_ATTACHMENT||renderbufferTarget!=GL_RENDERBUFFER){gl_error(GL_INVALID_ENUM);return;}if(!active->framebuffer||(renderbuffer&&!glIsRenderbuffer(renderbuffer))){gl_error(GL_INVALID_OPERATION);return;}struct lr_gpu_framebuffer p={.command=LR_GPU_FB_DEPTH_RENDERBUFFER,.name=active->framebuffer,.texture=renderbuffer,.format=GL_DEPTH_ATTACHMENT};framebuffer_command(&p);}
void glGetRenderbufferParameteriv(GLenum target,GLenum property,GLint *out){if(!renderbuffer_api(target))return;if(!out){gl_error(GL_INVALID_VALUE);return;}if(!active->renderbuffer){gl_error(GL_INVALID_OPERATION);return;}struct lr_gpu_renderbuffer p={.command=LR_GPU_RB_QUERY,.name=active->renderbuffer};if(!renderbuffer_command(&p))return;switch(property){case GL_RENDERBUFFER_WIDTH:*out=p.width;break;case GL_RENDERBUFFER_HEIGHT:*out=p.height;break;case GL_RENDERBUFFER_INTERNAL_FORMAT:*out=p.format;break;case GL_RENDERBUFFER_DEPTH_SIZE:*out=p.depth_bits;break;case GL_RENDERBUFFER_RED_SIZE:case GL_RENDERBUFFER_GREEN_SIZE:case GL_RENDERBUFFER_BLUE_SIZE:case GL_RENDERBUFFER_ALPHA_SIZE:case GL_RENDERBUFFER_STENCIL_SIZE:*out=0;break;default:gl_error(GL_INVALID_ENUM);}}
