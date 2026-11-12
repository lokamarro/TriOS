// Depth comparison, write masking and clear values are independent GL state.
static int update_depth(const struct lr_gpu_depth_state *state){
 struct lr_gpu_request r={.operation=LR_GPU_DEPTH_STATE,.address=(uintptr_t)state,.bytes=sizeof *state};
 if(!submit(&r))return 0;active->depth_state=*state;return 1;
}
void glDepthFunc(GLenum function){
 if(!active){gl_error(GL_INVALID_OPERATION);return;}
 if(function<0x0200||function>0x0207){gl_error(GL_INVALID_ENUM);return;}
 struct lr_gpu_depth_state s=active->depth_state;s.function=function;update_depth(&s);
}
void glDepthMask(GLboolean mask){
 if(!active){gl_error(GL_INVALID_OPERATION);return;}
 struct lr_gpu_depth_state s=active->depth_state;s.write_mask=!!mask;update_depth(&s);
}
void glClearDepth(double depth){
 if(!active){gl_error(GL_INVALID_OPERATION);return;}
 // Clamp infinities as GL specifies; reject NaN rather than submitting it.
 if(isnan(depth)){gl_error(GL_INVALID_VALUE);return;}
 struct lr_gpu_depth_state s=active->depth_state;s.clear_depth=fmax(0,fmin(1,depth));update_depth(&s);
}
void glClearDepthf(GLfloat depth){glClearDepth(depth);}
static int query_depth(GLenum property,double *value){
 switch(property){case 0x0b73:*value=active->depth_state.clear_depth;break;case 0x0b72:*value=active->depth_state.write_mask;break;case 0x0b74:*value=active->depth_state.function;break;case GL_DEPTH_TEST:*value=active->depth;break;default:gl_error(GL_INVALID_ENUM);return 0;}return 1;
}
void glGetDoublev(GLenum property,double *out){
 if(!active||!out){gl_error(GL_INVALID_OPERATION);return;}double value;if(query_depth(property,&value))*out=value;
}
void glGetFloatv(GLenum property,GLfloat *out){
 if(!active||!out){gl_error(GL_INVALID_OPERATION);return;}double value;if(query_depth(property,&value))*out=value;
}
void glGetBooleanv(GLenum property,unsigned char *out){
 if(!active||!out){gl_error(GL_INVALID_OPERATION);return;}double value;if(query_depth(property,&value))*out=value!=0;
}
