// Blend factors/equations become immutable Metal pipeline variants. Constant
// colors are encoder state; clears retain their GL behavior independent of blend.
static int blend_factor(GLenum f,int destination){return f==GL_ZERO||f==GL_ONE||(f>=0x300&&f<=0x308&&(!destination||f!=0x308))||(f>=0x8001&&f<=0x8004);}
static int blend_equation(GLenum f){return (f>=0x8006&&f<=0x8008)||f==0x800a||f==0x800b;}
static int update_blend(const struct lr_gpu_blend *state){struct lr_gpu_request r={.operation=LR_GPU_BLEND,.address=(uintptr_t)state,.bytes=sizeof(*state)};if(!submit(&r))return 0;active->blend=*state;return 1;}
void glBlendFuncSeparate(GLenum sr,GLenum dr,GLenum sa,GLenum da){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(!blend_factor(sr,0)||!blend_factor(dr,1)||!blend_factor(sa,0)||!blend_factor(da,1)){gl_error(GL_INVALID_ENUM);return;}struct lr_gpu_blend r=active->blend;r.src_rgb=sr;r.dst_rgb=dr;r.src_alpha=sa;r.dst_alpha=da;update_blend(&r);}
void glBlendFunc(GLenum source,GLenum destination){glBlendFuncSeparate(source,destination,source,destination);}
void glBlendEquationSeparate(GLenum rgb,GLenum alpha){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(!blend_equation(rgb)||!blend_equation(alpha)){gl_error(GL_INVALID_ENUM);return;}struct lr_gpu_blend r=active->blend;r.equation_rgb=rgb;r.equation_alpha=alpha;update_blend(&r);}
void glBlendEquation(GLenum equation){glBlendEquationSeparate(equation,equation);}
void glBlendColor(GLfloat red,GLfloat green,GLfloat blue,GLfloat alpha){if(!active){gl_error(GL_INVALID_OPERATION);return;}float color[]={red,green,blue,alpha};struct lr_gpu_blend r=active->blend;for(unsigned i=0;i<4;i++){if(!isfinite(color[i])){gl_error(GL_INVALID_VALUE);return;}r.color[i]=fmaxf(0,fminf(1,color[i]));}update_blend(&r);}
GLboolean glIsEnabled(GLenum cap){if(!active){gl_error(GL_INVALID_OPERATION);return GL_FALSE;}switch(cap){case GL_BLEND:return active->blend.enabled;case GL_DEPTH_TEST:return active->depth;case GL_CULL_FACE:return active->cull;case GL_LIGHTING:return active->lighting;case GL_LIGHT0:return active->light0;case GL_NORMALIZE:return active->normalize;default:gl_error(GL_INVALID_ENUM);return GL_FALSE;}}
