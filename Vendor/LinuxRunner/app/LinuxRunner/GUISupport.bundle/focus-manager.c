/* Minimal session window/focus manager, not a desktop. GPLv3/GPLv2. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
static Display *d; static Window root,check; static Atom protocols,take_focus,active,clock_atom,modal,state;
static int ignore_error(Display *unused,XErrorEvent *e){(void)unused;fprintf(stderr,"FOCUS: X error=%u request=%u resource=%lx\n",e->error_code,e->request_code,e->resourceid);return 0;}
static Window modal_for(Window parent){
    Window r,p,*children=NULL;unsigned count=0;Window result=parent;
    if(XQueryTree(d,root,&r,&p,&children,&count)){
        for(unsigned i=count;i>0;i--){Window child=children[i-1],owner;XWindowAttributes a;
            if(child==parent||!XGetWindowAttributes(d,child,&a)||a.map_state!=IsViewable||!XGetTransientForHint(d,child,&owner)||owner!=parent)continue;
            Atom actual;int format;unsigned long n,remaining;unsigned char *value=NULL;
            if(XGetWindowProperty(d,child,state,0,128,False,XA_ATOM,&actual,&format,&n,&remaining,&value)==Success&&format==32){
                Atom *atoms=(Atom*)value;for(unsigned long j=0;j<n;j++)if(atoms[j]==modal)result=child;
            }if(value)XFree(value);if(result!=parent)break;
        }XFree(children);
    }return result;
}
static void focus(Window w,Time time){
    w=modal_for(w);
    // ICCCM WM_TAKE_FOCUS needs a real X server timestamp, not CurrentTime.
    if(time==CurrentTime){static unsigned long tick;tick++;XEvent stamp;
        XChangeProperty(d,check,clock_atom,XA_INTEGER,32,PropModeReplace,(unsigned char*)&tick,1);
        XWindowEvent(d,check,PropertyChangeMask,&stamp);time=stamp.xproperty.time;
    }
    XWindowAttributes a;if(!XGetWindowAttributes(d,w,&a)||a.map_state!=IsViewable||a.override_redirect)return;
    XWMHints *h=XGetWMHints(d,w);if(!h||!(h->flags&InputHint)||h->input)XSetInputFocus(d,w,RevertToPointerRoot,time);if(h)XFree(h);
    Atom *p=0;int n=0;if(XGetWMProtocols(d,w,&p,&n)){for(int i=0;i<n;i++)if(p[i]==take_focus){XEvent e={0};e.xclient.type=ClientMessage;e.xclient.window=w;e.xclient.message_type=protocols;e.xclient.format=32;e.xclient.data.l[0]=take_focus;e.xclient.data.l[1]=time;XSendEvent(d,w,False,NoEventMask,&e);}XFree(p);}
    XChangeProperty(d,root,active,XA_WINDOW,32,PropModeReplace,(unsigned char*)&w,1);
    XRaiseWindow(d,w);XFlush(d);fprintf(stdout,"FOCUS: active=%lx time=%lu\n",w,time);fflush(stdout);
}
int main(int argc,char **argv){
    // The session starts us once the X socket file exists; Xvfb binds it
    // before it listens, so an early connection can be refused. Retry briefly.
    for(int i=0;i<100&&!(d=XOpenDisplay(0));i++){if(argc>1&&access(argv[1],F_OK)==0)return 0;usleep(100000);}
    if(!d){fprintf(stdout,"FOCUS: cannot open display %s\n",getenv("DISPLAY")?getenv("DISPLAY"):"(unset)");fflush(stdout);return 1;}
    root=DefaultRootWindow(d);XSetErrorHandler(ignore_error);
    protocols=XInternAtom(d,"WM_PROTOCOLS",False);take_focus=XInternAtom(d,"WM_TAKE_FOCUS",False);active=XInternAtom(d,"_NET_ACTIVE_WINDOW",False);
    XSelectInput(d,root,SubstructureRedirectMask|SubstructureNotifyMask);
    check=XCreateSimpleWindow(d,root,-10,-10,1,1,0,0,0);Atom wmcheck=XInternAtom(d,"_NET_SUPPORTING_WM_CHECK",False);
    clock_atom=XInternAtom(d,"_LINUXRUNNER_FOCUS_CLOCK",False);modal=XInternAtom(d,"_NET_WM_STATE_MODAL",False);state=XInternAtom(d,"_NET_WM_STATE",False);
    XSelectInput(d,check,PropertyChangeMask);
    XChangeProperty(d,root,wmcheck,XA_WINDOW,32,PropModeReplace,(unsigned char*)&check,1);XChangeProperty(d,check,wmcheck,XA_WINDOW,32,PropModeReplace,(unsigned char*)&check,1);
    const char *name="LinuxRunner session focus manager";XChangeProperty(d,check,XInternAtom(d,"_NET_WM_NAME",False),XInternAtom(d,"UTF8_STRING",False),8,PropModeReplace,(const unsigned char*)name,strlen(name));
    XChangeProperty(d,root,XInternAtom(d,"_NET_SUPPORTED",False),XA_ATOM,32,PropModeReplace,(unsigned char*)&active,1);
    XSync(d,False);puts("FOCUS: ready (map/pointer focus, configure, WM_TAKE_FOCUS)");fflush(stdout);
    while(argc<2||access(argv[1],F_OK)!=0){
        if(!XPending(d)){struct pollfd fd={.fd=ConnectionNumber(d),.events=POLLIN};poll(&fd,1,100);continue;}
        XEvent e;XNextEvent(d,&e);
        if(e.type==MapRequest){Window w=e.xmaprequest.window;XMapWindow(d,w);XSelectInput(d,w,EnterWindowMask);XSync(d,False);focus(w,CurrentTime);}
        else if(e.type==EnterNotify && e.xcrossing.mode==NotifyNormal)focus(e.xcrossing.window,e.xcrossing.time);
        else if(e.type==ConfigureRequest){XConfigureRequestEvent *c=&e.xconfigurerequest;XWindowChanges v={.x=c->x,.y=c->y,.width=c->width,.height=c->height,.border_width=c->border_width,.sibling=c->above,.stack_mode=c->detail};XConfigureWindow(d,c->window,c->value_mask,&v);XFlush(d);}
        else if(e.type==ClientMessage&&e.xclient.message_type==active)focus(e.xclient.window,CurrentTime);
        else if(e.type==UnmapNotify){Window parent,*children;unsigned count;Window r;if(XQueryTree(d,root,&r,&parent,&children,&count)){for(unsigned i=count;i>0;i--){XWindowAttributes a;if(XGetWindowAttributes(d,children[i-1],&a)&&a.map_state==IsViewable&&!a.override_redirect){focus(children[i-1],CurrentTime);break;}}XFree(children);}}
    }
    XCloseDisplay(d);puts("FOCUS: clean exit");return 0;
}
