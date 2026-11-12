// Explicit opt-in client GLX provider. It owns its contexts, rather than
// borrowing Mesa contexts. This limited provider does not advertise a desktop
// GL version, GLX_ARB_create_context, sharing, or GLX conformance.
#include <GL/glx.h>
#include <GL/glxext.h>
struct lr_glx_config { Display *display;int screen;VisualID visual; };
struct lr_glx_context {uintptr_t handle;Display *display;int screen;EGLContext context;EGLSurface surface;GLXDrawable drawable;long owner;int deleted;};
static struct lr_glx_config glx_configs[MAX_OBJECTS];
static struct lr_glx_context glx_contexts[MAX_OBJECTS];
static pthread_mutex_t glx_objects=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct lr_glx_context *glx_current;
static uintptr_t glx_next=4096;
static struct lr_glx_context *glx_find(GLXContext h){for(int i=0;i<MAX_OBJECTS;i++)if(glx_contexts[i].handle==(uintptr_t)h&&h)return &glx_contexts[i];return NULL;}
static struct lr_glx_config *glx_config(Display *d,GLXFBConfig h){for(int i=0;i<MAX_OBJECTS;i++)if((void*)&glx_configs[i]==(void*)h&&glx_configs[i].display==d)return &glx_configs[i];return NULL;}
static struct lr_glx_config *glx_default_config(Display *d,int screen){
 if(!d||screen<0||screen>=ScreenCount(d))return NULL;
 Visual *v=DefaultVisual(d,screen);if(v->class!=TrueColor||v->red_mask!=0xff0000||v->green_mask!=0xff00||v->blue_mask!=0xff||DefaultDepth(d,screen)!=24)return NULL;
 for(int i=0;i<MAX_OBJECTS;i++)if(glx_configs[i].display==d&&glx_configs[i].screen==screen)return &glx_configs[i];
 for(int i=0;i<MAX_OBJECTS;i++)if(!glx_configs[i].display){glx_configs[i]=(struct lr_glx_config){d,screen,XVisualIDFromVisual(v)};return &glx_configs[i];}return NULL;
}
static int glx_attribute(struct lr_glx_config *c,int a,int *out){
 if(!c||!out)return GLX_BAD_ATTRIBUTE;
 switch(a){case GLX_USE_GL:case GLX_RGBA:case GLX_DOUBLEBUFFER:case GLX_X_RENDERABLE:*out=True;break;
 case GLX_BUFFER_SIZE:*out=32;break;case GLX_RED_SIZE:case GLX_GREEN_SIZE:case GLX_BLUE_SIZE:case GLX_ALPHA_SIZE:*out=8;break;
 case GLX_DEPTH_SIZE:*out=24;break;case GLX_STENCIL_SIZE:case GLX_AUX_BUFFERS:case GLX_STEREO:case GLX_ACCUM_RED_SIZE:case GLX_ACCUM_GREEN_SIZE:case GLX_ACCUM_BLUE_SIZE:case GLX_ACCUM_ALPHA_SIZE:case GLX_SAMPLE_BUFFERS:case GLX_SAMPLES:*out=0;break;
 case GLX_RENDER_TYPE:*out=GLX_RGBA_BIT;break;case GLX_DRAWABLE_TYPE:*out=GLX_WINDOW_BIT;break;case GLX_X_VISUAL_TYPE:*out=GLX_TRUE_COLOR;break;
 case GLX_CONFIG_CAVEAT:*out=GLX_NONE;break;case GLX_FBCONFIG_ID:*out=1+c->screen;break;case GLX_VISUAL_ID:*out=c->visual;break;case GLX_SCREEN:*out=c->screen;break;
 default:return GLX_BAD_ATTRIBUTE;}return Success;
}
static int glx_matches(struct lr_glx_config *c,const int *a,int legacy){
 if(!a)return 1;
 for(unsigned i=0;i<128;){int key=a[i++];if(key==None)return 1;int actual,value;
 if(legacy&&(key==GLX_RGBA||key==GLX_DOUBLEBUFFER||key==GLX_STEREO)){value=True;}else {if(i==128)return 0;value=a[i++];}
 if(glx_attribute(c,key,&actual)!=Success)return 0;
 if(key==GLX_RED_SIZE||key==GLX_GREEN_SIZE||key==GLX_BLUE_SIZE||key==GLX_ALPHA_SIZE||key==GLX_DEPTH_SIZE||key==GLX_STENCIL_SIZE||key==GLX_BUFFER_SIZE||key==GLX_AUX_BUFFERS||key==GLX_ACCUM_RED_SIZE||key==GLX_ACCUM_GREEN_SIZE||key==GLX_ACCUM_BLUE_SIZE||key==GLX_ACCUM_ALPHA_SIZE||key==GLX_SAMPLES){if(value<0||value>actual)return 0;}
 else if(key==GLX_RENDER_TYPE||key==GLX_DRAWABLE_TYPE){if(value&~actual)return 0;}
 else if(value!=GLX_DONT_CARE&&value!=actual)return 0;
 }return 0;
}
XVisualInfo *glXChooseVisual(Display *d,int screen,int *attributes){pthread_mutex_lock(&glx_objects);struct lr_glx_config *c=glx_default_config(d,screen);XVisualInfo *v=NULL;if(c&&glx_matches(c,attributes,1)){XVisualInfo t={.visualid=c->visual,.screen=screen};int n;v=XGetVisualInfo(d,VisualIDMask|VisualScreenMask,&t,&n);}pthread_mutex_unlock(&glx_objects);return v;}
GLXFBConfig *glXChooseFBConfig(Display *d,int screen,const int *a,int *n){if(!n)return NULL;*n=0;pthread_mutex_lock(&glx_objects);struct lr_glx_config *c=glx_default_config(d,screen);GLXFBConfig *result=NULL;if(c&&glx_matches(c,a,0)){result=malloc(sizeof *result);if(result){result[0]=(GLXFBConfig)c;*n=1;}}pthread_mutex_unlock(&glx_objects);return result;}
GLXFBConfig *glXGetFBConfigs(Display *d,int screen,int *n){return glXChooseFBConfig(d,screen,NULL,n);}
int glXGetFBConfigAttrib(Display *d,GLXFBConfig h,int a,int *v){pthread_mutex_lock(&glx_objects);int result=glx_attribute(glx_config(d,h),a,v);pthread_mutex_unlock(&glx_objects);return result;}
XVisualInfo *glXGetVisualFromFBConfig(Display *d,GLXFBConfig h){pthread_mutex_lock(&glx_objects);struct lr_glx_config *c=glx_config(d,h);XVisualInfo *v=NULL;if(c){XVisualInfo t={.visualid=c->visual,.screen=c->screen};int n;v=XGetVisualInfo(d,VisualIDMask|VisualScreenMask,&t,&n);}pthread_mutex_unlock(&glx_objects);return v;}
int glXGetConfig(Display *d,XVisualInfo *v,int a,int *out){if(!v)return GLX_BAD_VISUAL;pthread_mutex_lock(&glx_objects);struct lr_glx_config *c=glx_default_config(d,v->screen);int result=!c||c->visual!=v->visualid?GLX_BAD_VISUAL:glx_attribute(c,a,out);pthread_mutex_unlock(&glx_objects);return result;}
static GLXContext glx_create(Display *d,int screen){
 if(!eglInitialize(DISPLAY,NULL,NULL))return NULL;
 EGLContext egl=eglCreateContext(DISPLAY,CONFIG,NULL,NULL);if(!egl)return NULL;
 for(int i=0;i<MAX_OBJECTS;i++)if(!glx_contexts[i].handle){glx_contexts[i]=(struct lr_glx_context){.handle=glx_next++,.display=d,.screen=screen,.context=egl};struct context *c=find_context(egl);c->desktop=1;return (GLXContext)glx_contexts[i].handle;}
 eglDestroyContext(DISPLAY,egl);return NULL;
}
GLXContext glXCreateContext(Display *d,XVisualInfo *v,GLXContext shared,Bool direct){if(!d||!v||shared||!direct)return NULL;pthread_mutex_lock(&glx_objects);struct lr_glx_config *c=glx_default_config(d,v->screen);GLXContext result=c&&v->visualid==c->visual?glx_create(d,v->screen):NULL;pthread_mutex_unlock(&glx_objects);return result;}
GLXContext glXCreateNewContext(Display *d,GLXFBConfig h,int type,GLXContext shared,Bool direct){if(shared||!direct||type!=GLX_RGBA_TYPE)return NULL;pthread_mutex_lock(&glx_objects);struct lr_glx_config *c=glx_config(d,h);GLXContext result=c?glx_create(d,c->screen):NULL;pthread_mutex_unlock(&glx_objects);return result;}
GLXContext glXCreateContextAttribsARB(Display *d,GLXFBConfig h,GLXContext shared,Bool direct,const int *a){
 // No numeric desktop GL version is promised. Requests carrying version,
 // profile, robustness or sharing requirements must not get an ES context.
 if(a&&a[0]!=None){fprintf(stderr,"GPU_GLX_CONTEXT_REJECT");for(unsigned i=0;i<32&&a[i]!=None;i+=2)fprintf(stderr," attr_0x%x=%d",a[i],a[i+1]);fprintf(stderr," unsupported desktop capabilities; no Mesa fallback\n");return NULL;}
 return glXCreateNewContext(d,h,GLX_RGBA_TYPE,shared,direct);
}
static void glx_release(struct lr_glx_context *c){if(c->surface)eglDestroySurface(DISPLAY,c->surface);eglDestroyContext(DISPLAY,c->context);memset(c,0,sizeof *c);}
Bool glXMakeContextCurrent(Display *d,GLXDrawable draw,GLXDrawable read,GLXContext h){
 pthread_mutex_lock(&glx_objects);Bool result=False;struct lr_glx_context *c=glx_find(h);long tid=syscall(SYS_gettid);
 if(!h){if(draw||read||!d||(glx_current&&glx_current->display!=d))goto done;if(!eglMakeCurrent(DISPLAY,NULL,NULL,NULL))goto done;
 if(glx_current){struct lr_glx_context *old=glx_current;old->owner=0;glx_current=NULL;if(old->deleted)glx_release(old);}result=True;goto done;}
 if(!c||c->deleted||c->display!=d||!draw||draw!=read||(c->owner&&c->owner!=tid))goto done;
 XWindowAttributes a;if(!XGetWindowAttributes(d,draw,&a)||a.screen!=ScreenOfDisplay(d,c->screen)||a.visual!=DefaultVisual(d,c->screen)||a.depth!=24||a.width<1||a.height<1||a.width>LR_GPU_MAX_DIMENSION||a.height>LR_GPU_MAX_DIMENSION)goto done;
 // A drawable cannot be concurrently rendered by another context.
 for(int i=0;i<MAX_OBJECTS;i++)if(&glx_contexts[i]!=c&&glx_contexts[i].display==d&&glx_contexts[i].drawable==draw&&glx_contexts[i].owner&& &glx_contexts[i]!=glx_current)goto done;
 if(c->surface&&c->drawable!=draw){if(c->owner){if(!eglMakeCurrent(DISPLAY,NULL,NULL,NULL))goto done;c->owner=0;glx_current=NULL;}if(!eglDestroySurface(DISPLAY,c->surface))goto done;c->surface=NULL;c->drawable=None;}
 if(!c->surface){c->surface=create_window_surface(d,DISPLAY,CONFIG,draw,NULL);if(!c->surface)goto done;c->drawable=draw;}
 if(!eglMakeCurrent(DISPLAY,c->surface,c->surface,c->context))goto done;
 if(glx_current&&glx_current!=c){struct lr_glx_context *old=glx_current;old->owner=0;if(old->deleted)glx_release(old);}
 c->owner=tid;glx_current=c;result=True;
 done:pthread_mutex_unlock(&glx_objects);return result;
}
Bool glXMakeCurrent(Display *d,GLXDrawable w,GLXContext c){return glXMakeContextCurrent(d,w,w,c);}
GLXContext glXGetCurrentContext(void){return glx_current?(GLXContext)glx_current->handle:NULL;}
GLXDrawable glXGetCurrentDrawable(void){return glx_current?glx_current->drawable:None;}
GLXDrawable glXGetCurrentReadDrawable(void){return glXGetCurrentDrawable();}
Display *glXGetCurrentDisplay(void){return glx_current?glx_current->display:NULL;}
void glXDestroyContext(Display *d,GLXContext h){pthread_mutex_lock(&glx_objects);struct lr_glx_context *c=glx_find(h);if(c&&c->display==d){if(c->owner)c->deleted=1;else glx_release(c);}pthread_mutex_unlock(&glx_objects);}
Bool glXIsDirect(Display *d,GLXContext h){pthread_mutex_lock(&glx_objects);struct lr_glx_context *c=glx_find(h);Bool result=c&&c->display==d&&!c->deleted;pthread_mutex_unlock(&glx_objects);return result;}
void glXSwapBuffers(Display *d,GLXDrawable w){if(!glx_current||glx_current->display!=d||glx_current->drawable!=w){gl_error(GL_INVALID_OPERATION);return;}
 struct surface *s=find_surface(glx_current->surface);XWindowAttributes a;if(!s||!XGetWindowAttributes(d,w,&a)){gl_error(GL_INVALID_OPERATION);return;}
 if((s->width!=(unsigned)a.width||s->height!=(unsigned)a.height)&&!resize_surface(active,s,a.width,a.height)){gl_error(GL_OUT_OF_MEMORY);return;}
 if(!eglSwapBuffers(DISPLAY,glx_current->surface))gl_error(GL_INVALID_OPERATION);
}
Bool glXQueryExtension(Display *d,int *error,int *event){if(!d)return False;int op,e,v;if(!XQueryExtension(d,"GLX",&op,&e,&v))return False;if(error)*error=e;if(event)*event=v;return True;}
Bool glXQueryVersion(Display *d,int *major,int *minor){if(!d)return False;if(major)*major=1;if(minor)*minor=0;return True;}
const char *glXQueryExtensionsString(Display *d,int screen){return d&&screen>=0&&screen<ScreenCount(d)?"":NULL;}
const char *glXGetClientString(Display *d,int name){if(!d)return NULL;switch(name){case GLX_VENDOR:return "LinuxRunner experimental Metal GLX provider";case GLX_VERSION:return "1.0 limited non-conformant client subset";case GLX_EXTENSIONS:return "";default:return NULL;}}
const char *glXQueryServerString(Display *d,int screen,int name){(void)d;(void)screen;(void)name;return NULL; /* server capabilities are not this client provider's capabilities */}
// Immediate triangles reuse the existing Metal fixed-function pipeline. No
// display-list or complete compatibility-profile implementation is claimed.
void glBegin(GLenum mode){if(!active||active->immediate){gl_error(GL_INVALID_OPERATION);return;}if(mode!=GL_TRIANGLES&&mode!=GL_TRIANGLE_STRIP&&mode!=GL_TRIANGLE_FAN){gl_error(GL_INVALID_ENUM);return;}if(!active->immediate_data){active->immediate_data=malloc(LR_GPU_MAX_VERTICES*sizeof *active->immediate_data);if(!active->immediate_data){gl_error(GL_OUT_OF_MEMORY);return;}}active->immediate=1;active->immediate_mode=mode;active->immediate_count=0;}
void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a){float v[]={r,g,b,a};if(!active){gl_error(GL_INVALID_OPERATION);return;}memcpy(active->immediate_color,v,sizeof v);}
void glColor3f(GLfloat r,GLfloat g,GLfloat b){glColor4f(r,g,b,1);}
void glVertex3f(GLfloat x,GLfloat y,GLfloat z){if(!active||!active->immediate){gl_error(GL_INVALID_OPERATION);return;}if(active->immediate_count==LR_GPU_MAX_VERTICES){gl_error(GL_OUT_OF_MEMORY);return;}float p[]={x,y,z,1};memcpy(active->immediate_data[active->immediate_count].position,p,sizeof p);memcpy(active->immediate_data[active->immediate_count++].color,active->immediate_color,sizeof active->immediate_color);}
void glVertex2f(GLfloat x,GLfloat y){glVertex3f(x,y,0);}
void glEnd(void){if(!active||!active->immediate){gl_error(GL_INVALID_OPERATION);return;}active->immediate=0;
 struct {const char *vertex,*color;GLint vertex_size;GLenum vertex_type,color_type;GLsizei vertex_stride,color_stride;unsigned vertex_buffer,color_buffer;int vertex_enabled,color_enabled;} saved={active->vertex,active->color,active->vertex_size,active->vertex_type,active->color_type,active->vertex_stride,active->color_stride,active->vertex_buffer,active->color_buffer,active->vertex_enabled,active->color_enabled};active->vertex=(void*)active->immediate_data;active->color=(void*)active->immediate_data[0].color;active->vertex_size=4;active->vertex_type=active->color_type=GL_FLOAT;active->vertex_stride=active->color_stride=sizeof *active->immediate_data;active->vertex_buffer=active->color_buffer=0;active->vertex_enabled=active->color_enabled=1;draw(active->immediate_mode,0,active->immediate_count,0,NULL);
 active->vertex=saved.vertex;active->color=saved.color;active->vertex_size=saved.vertex_size;active->vertex_type=saved.vertex_type;active->color_type=saved.color_type;active->vertex_stride=saved.vertex_stride;active->color_stride=saved.color_stride;active->vertex_buffer=saved.vertex_buffer;active->color_buffer=saved.color_buffer;active->vertex_enabled=saved.vertex_enabled;active->color_enabled=saved.color_enabled;
}
void glOrtho(GLdouble l,GLdouble r,GLdouble b,GLdouble t,GLdouble n,GLdouble f){if(!active){gl_error(GL_INVALID_OPERATION);return;}if(l==r||b==t||n==f){gl_error(GL_INVALID_VALUE);return;}float m[16]={0};m[0]=2/(r-l);m[5]=2/(t-b);m[10]=-2/(f-n);m[12]=-(r+l)/(r-l);m[13]=-(t+b)/(t-b);m[14]=-(f+n)/(f-n);m[15]=1;glMultMatrixf(m);}
void glGenVertexArrays(GLsizei n,GLuint *names){if(!active||!active->desktop||!names){gl_error(GL_INVALID_OPERATION);return;}if(n<0||n>16){gl_error(GL_INVALID_VALUE);return;}for(int k=0;k<n;k++){names[k]=0;for(unsigned i=1;i<17;i++)if(!active->vao[i].reserved){active->vao[i].reserved=1;for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++){active->vao[i].attribute[a].size=4;active->vao[i].attribute[a].type=GL_FLOAT;active->vao[i].attribute[a].stride=16;}names[k]=i;break;}if(!names[k])gl_error(GL_OUT_OF_MEMORY);}}
void glBindVertexArray(GLuint name){if(!active||!active->desktop){gl_error(GL_INVALID_OPERATION);return;}if(name>=17||(name&&!active->vao[name].reserved)){gl_error(GL_INVALID_OPERATION);return;}
 struct lr_vertex_array *old=&active->vao[active->vertex_array];memcpy(old->attribute,active->attribute,sizeof active->attribute);old->element=active->bound_element;
 float values[LR_GL_MAX_ATTRIBUTES][4];for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)memcpy(values[a],active->attribute[a].value,16);
 memcpy(active->attribute,active->vao[name].attribute,sizeof active->attribute);for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)memcpy(active->attribute[a].value,values[a],16);
 active->bound_element=active->vao[name].element;active->vertex_array=name;active->vao[name].bound=1;collect_buffers();
}
void glDeleteVertexArrays(GLsizei n,const GLuint *names){if(!active||!active->desktop||!names){gl_error(GL_INVALID_OPERATION);return;}if(n<0){gl_error(GL_INVALID_VALUE);return;}for(int k=0;k<n;k++){unsigned name=names[k];if(!name||name>=17||!active->vao[name].reserved)continue;if(active->vertex_array==name)glBindVertexArray(0);memset(&active->vao[name],0,sizeof active->vao[name]);}collect_buffers();}
GLboolean glIsVertexArray(GLuint name){return active&&active->desktop&&name>0&&name<17&&active->vao[name].reserved&&active->vao[name].bound;}
__GLXextFuncPtr glXGetProcAddress(const GLubyte *n){if(!n)return NULL;const char *name=(const char*)n;
#define LR_GLX_PROC(f) if(!strcmp(name,#f))return (__GLXextFuncPtr)f
 LR_GLX_PROC(glXChooseVisual);LR_GLX_PROC(glXChooseFBConfig);LR_GLX_PROC(glXGetFBConfigs);LR_GLX_PROC(glXGetFBConfigAttrib);LR_GLX_PROC(glXGetVisualFromFBConfig);LR_GLX_PROC(glXGetConfig);LR_GLX_PROC(glXCreateContext);LR_GLX_PROC(glXCreateNewContext);LR_GLX_PROC(glXCreateContextAttribsARB);LR_GLX_PROC(glXMakeCurrent);LR_GLX_PROC(glXMakeContextCurrent);LR_GLX_PROC(glXGetCurrentContext);LR_GLX_PROC(glXGetCurrentDrawable);LR_GLX_PROC(glXGetCurrentReadDrawable);LR_GLX_PROC(glXGetCurrentDisplay);LR_GLX_PROC(glXSwapBuffers);LR_GLX_PROC(glXDestroyContext);LR_GLX_PROC(glXIsDirect);LR_GLX_PROC(glXQueryVersion);LR_GLX_PROC(glXQueryExtension);LR_GLX_PROC(glXQueryExtensionsString);LR_GLX_PROC(glXGetClientString);LR_GLX_PROC(glXQueryServerString);LR_GLX_PROC(glXGetProcAddress);LR_GLX_PROC(glXGetProcAddressARB);LR_GLX_PROC(glBegin);LR_GLX_PROC(glEnd);LR_GLX_PROC(glColor3f);LR_GLX_PROC(glColor4f);LR_GLX_PROC(glVertex2f);LR_GLX_PROC(glVertex3f);LR_GLX_PROC(glGenVertexArrays);LR_GLX_PROC(glBindVertexArray);LR_GLX_PROC(glDeleteVertexArrays);LR_GLX_PROC(glIsVertexArray);LR_GLX_PROC(glOrtho);LR_GLX_PROC(glMatrixMode);LR_GLX_PROC(glLoadIdentity);LR_GLX_PROC(glPushMatrix);LR_GLX_PROC(glPopMatrix);LR_GLX_PROC(glRotatef);LR_GLX_PROC(glTranslatef);LR_GLX_PROC(glScalef);LR_GLX_PROC(glShadeModel);
#undef LR_GLX_PROC
 return (__GLXextFuncPtr)eglGetProcAddress(name);
}
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name){return glXGetProcAddress(name);}
