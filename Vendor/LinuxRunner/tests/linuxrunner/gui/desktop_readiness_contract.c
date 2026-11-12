#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdio.h>
int main(void){
 Display*d=XOpenDisplay(0);assert(d);Window r=DefaultRootWindow(d);
 Window owner=XCreateSimpleWindow(d,r,0,0,1,1,0,0,0);XSetSelectionOwner(d,XInternAtom(d,"WM_S0",False),owner,CurrentTime);
 Window panel=XCreateSimpleWindow(d,r,0,0,1,1,0,0,0),surface=XCreateSimpleWindow(d,r,0,0,1,1,0,0,0);
 Atom type=XInternAtom(d,"_NET_WM_WINDOW_TYPE",False),dock=XInternAtom(d,"_NET_WM_WINDOW_TYPE_DOCK",False),desk=XInternAtom(d,"_NET_WM_WINDOW_TYPE_DESKTOP",False);
 XChangeProperty(d,panel,type,XA_ATOM,32,PropModeReplace,(void*)&dock,1);XChangeProperty(d,surface,type,XA_ATOM,32,PropModeReplace,(void*)&desk,1);XMapWindow(d,panel);XMapWindow(d,surface);XSync(d,False);
 int s=system("/home/mobile/Downloads/.linuxrunner/gui/desktop-probe --ready");assert(WIFEXITED(s)&&WEXITSTATUS(s)==1);
 XResizeWindow(d,panel,640,36);XResizeWindow(d,surface,640,480);XSync(d,False);
 s=system("/home/mobile/Downloads/.linuxrunner/gui/desktop-probe --ready");assert(WIFEXITED(s)&&WEXITSTATUS(s)==0);
 XCloseDisplay(d);puts("DESKTOP_READINESS_CONTRACT PASS tiny-helpers-rejected full-surface-accepted");return 0;
}
