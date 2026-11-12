#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"XSHM_FAIL line=%d %s errno=%d xerror=%d\n",__LINE__,#x,errno,error);exit(1);}}while(0)
static int error;
static int handler(Display *d,XErrorEvent *e){(void)d;error=e->error_code;return 0;}
int main(void){Display *d=XOpenDisplay(NULL),*barrier=XOpenDisplay(NULL);CHECK(d&&barrier);XSetErrorHandler(handler);CHECK(XShmQueryExtension(d));
 int screen=DefaultScreen(d),depth=DefaultDepth(d,screen);Visual *v=DefaultVisual(d,screen);CHECK(v->class==TrueColor);
 Pixmap p=XCreatePixmap(d,DefaultRootWindow(d),69,45,depth);GC gc=XCreateGC(d,p,0,0);
 for(unsigned run=0;run<8;run++){
  XSetForeground(d,gc,0x123456);XFillRectangle(d,p,gc,0,0,69,45);XSetForeground(d,gc,0xabcdef);XFillRectangle(d,p,gc,17,9,35,25);XSync(d,False);CHECK(!error);
  XShmSegmentInfo segment={0};XImage *image=XShmCreateImage(d,v,depth,ZPixmap,NULL,&segment,69,45);CHECK(image);
  segment.shmid=shmget(IPC_PRIVATE,(size_t)image->bytes_per_line*image->height,IPC_CREAT|0600);CHECK(segment.shmid>=0);
  segment.shmaddr=shmat(segment.shmid,NULL,0);CHECK(segment.shmaddr!=(void*)-1);image->data=segment.shmaddr;segment.readOnly=False;
  /* Delay the server-side attach until after IPC_RMID. Linux still permits
     that attach while the client owns the segment; no timing guess needed. */
  XGrabServer(barrier);XSync(barrier,False);CHECK(XShmAttach(d,&segment));XFlush(d);CHECK(shmctl(segment.shmid,IPC_RMID,NULL)==0);XUngrabServer(barrier);XSync(barrier,False);XSync(d,False);CHECK(!error);
  CHECK(XShmGetImage(d,p,image,0,0,AllPlanes));XImage *normal=XGetImage(d,p,0,0,69,45,AllPlanes,ZPixmap);CHECK(normal);
  for(int y=0;y<45;y++)for(int x=0;x<69;x++){unsigned long expected=(x>=17&&x<52&&y>=9&&y<34)?0xabcdef:0x123456;CHECK(XGetPixel(normal,x,y)==expected&&XGetPixel(image,x,y)==expected);}
  XDestroyImage(normal);XPutPixel(image,68,44,0xa1b2c3);CHECK(XShmPutImage(d,p,gc,image,0,0,0,0,69,45,False));XSync(d,False);CHECK(!error);
  normal=XGetImage(d,p,68,44,1,1,AllPlanes,ZPixmap);CHECK(normal&&XGetPixel(normal,0,0)==0xa1b2c3);XDestroyImage(normal);
  XShmDetach(d,&segment);XSync(d,False);CHECK(!error);image->data=NULL;XDestroyImage(image);CHECK(shmdt(segment.shmaddr)==0);
  struct shmid_ds state;CHECK(shmctl(segment.shmid,IPC_STAT,&state)==-1&&(errno==EINVAL||errno==EIDRM));
 }
 XFreeGC(d,gc);XFreePixmap(d,p);XCloseDisplay(barrier);XCloseDisplay(d);puts("XSHM_CONTRACT_PASS delayed_attach_after_RMID get_pixels put_pixels cross_page eight_cycles cleanup");return 0;
}
