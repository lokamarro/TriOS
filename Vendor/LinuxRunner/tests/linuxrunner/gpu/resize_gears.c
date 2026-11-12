// External X11 client exercises the unchanged application's resize handling.
#include <X11/Xlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static Window find(Display *d,Window root,int depth){if(depth>4)return 0;char *name=NULL;if(XFetchName(d,root,&name)&&name){int match=!strcmp(name,"gears")||!strcmp(name,"es2gears");XFree(name);if(match)return root;}Window r,p,*children=NULL;unsigned n=0;if(!XQueryTree(d,root,&r,&p,&children,&n))return 0;Window result=0;for(unsigned i=0;i<n&&!result&&i<1000;i++)result=find(d,children[i],depth+1);if(children)XFree(children);return result;}
int main(void){Display *d=XOpenDisplay(NULL);assert(d);Window w=0;for(int i=0;i<20&&!w;i++){w=find(d,DefaultRootWindow(d),0);if(!w)sleep(1);}assert(w);unsigned sizes[][2]={{420,320},{160,240},{300,300}};for(int i=0;i<3;i++){XResizeWindow(d,w,sizes[i][0],sizes[i][1]);XSync(d,0);sleep(3);}puts("GPU_X11_RESIZE_REQUESTS_SENT");XCloseDisplay(d);}
