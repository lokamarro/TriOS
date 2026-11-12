#define _GNU_SOURCE
/* Explicit diagnostic only: bounded metadata/active poll arguments; no environ or payload dump. */
#include <dirent.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void copy(FILE *out,const char *path,size_t limit){
 FILE *in=fopen(path,"r");if(!in){fprintf(out," unavailable errno=%d\n",errno);return;}
 char b[4096];size_t n;while(limit&&(n=fread(b,1,limit<sizeof b?limit:sizeof b,in))){fwrite(b,1,n,out);limit-=n;}fclose(in);
 if(!limit)fputs("\nSNAPSHOT_TRUNCATED\n",out);
}
/* Only inspect a tracked active ppoll wait, through normal proc permission checks.
 * Last-syscall registers outside a tracked wait are not coherent arguments. */
static void poll_wait_arguments(FILE *out){
 FILE *tasks=fopen("/proc/ish/tasks","r");if(!tasks)return;
 char line[512];int tid=0,waiting=0,syscall=0,matched=0;unsigned inspected=0;
 while(inspected<64&&fgets(line,sizeof line,tasks)){
  int id=0,pid=0,ppid=0,zombie=0;char comm[32];
  if(sscanf(line,"TASK: tid=%d pid=%d ppid=%d comm=%31s instrumented_wait=%d zombie=%d last_syscall=%d",&id,&pid,&ppid,comm,&waiting,&zombie,&syscall)==7){tid=id;matched=waiting&&syscall==73;continue;}
  unsigned long long lr=0,a0=0,a1=0,a2=0,a3=0,a8=0;
  if(!matched||sscanf(line,"TASK_REGS: tid=%d lr=%llx x0=%llx x1=%llx x2=%llx x3=%llx x8=%llx",&id,&lr,&a0,&a1,&a2,&a3,&a8)!=7||id!=tid)continue;
  matched=0;inspected++;if(a8!=73||a1>64)continue;
  char path[80];snprintf(path,sizeof path,"/proc/%d/mem",tid);int mem=open(path,O_RDONLY|O_CLOEXEC);
  if(mem<0){fprintf(out,"POLL_ARGUMENTS tid=%d unavailable errno=%d\n",tid,errno);continue;}
  struct {int32_t fd;int16_t events,revents;} fds[64];
  _Static_assert(sizeof fds[0]==8,"Linux pollfd ABI");
  size_t size=a1*sizeof fds[0];ssize_t n=size?pread(mem,fds,size,(off_t)a0):0;
  if(n==(ssize_t)size){
   fprintf(out,"POLL_ARGUMENTS tid=%d nfds=%llu best_effort=1\n",tid,a1);
   for(unsigned i=0;i<a1;i++)fprintf(out,"POLL_DESCRIPTOR tid=%d fd=%d events=%#x revents=%#x\n",tid,fds[i].fd,(unsigned short)fds[i].events,(unsigned short)fds[i].revents);
  }else fprintf(out,"POLL_ARGUMENTS tid=%d read_unavailable bytes=%zd errno=%d\n",tid,n,n<0?errno:0);
  if(!a2)fprintf(out,"POLL_TIMEOUT tid=%d infinite=1\n",tid);
  else {int64_t ts[2];if(pread(mem,ts,sizeof ts,(off_t)a2)==sizeof ts)fprintf(out,"POLL_TIMEOUT tid=%d seconds=%lld nanoseconds=%lld\n",tid,(long long)ts[0],(long long)ts[1]);}
  close(mem);
 }
 fclose(tasks);
}
/* A tracked FUTEX_WAIT whose word no longer equals the awaited value is only a
 * candidate lost wakeup: the sample is not atomic, so require it to persist. */
static void futex_wait_words(FILE *out){
 FILE *tasks=fopen("/proc/ish/tasks","r");if(!tasks)return;
 char line[512];int tid=0,waiting=0,syscall=0,matched=0;unsigned inspected=0;
 while(inspected<512&&fgets(line,sizeof line,tasks)){
  int id=0,pid=0,ppid=0,zombie=0;char comm[32];
  if(sscanf(line,"TASK: tid=%d pid=%d ppid=%d comm=%31s instrumented_wait=%d zombie=%d last_syscall=%d",&id,&pid,&ppid,comm,&waiting,&zombie,&syscall)==7){tid=id;matched=waiting&&syscall==98;continue;}
  unsigned long long lr=0,a0=0,a1=0,a2=0,a3=0,a8=0;
  if(!matched||sscanf(line,"TASK_REGS: tid=%d lr=%llx x0=%llx x1=%llx x2=%llx x3=%llx x8=%llx",&id,&lr,&a0,&a1,&a2,&a3,&a8)!=7||id!=tid)continue;
  matched=0;unsigned op=a1&0x7f;if(a8!=98||(op!=0&&op!=9))continue;inspected++;
  char path[80];snprintf(path,sizeof path,"/proc/%d/mem",tid);int mem=open(path,O_RDONLY|O_CLOEXEC);
  uint32_t word=0;ssize_t n=mem<0?-1:pread(mem,&word,sizeof word,(off_t)a0);if(mem>=0)close(mem);
  if(n!=(ssize_t)sizeof word){fprintf(out,"FUTEX_WORD tid=%d uaddr=%#llx unavailable errno=%d\n",tid,a0,n<0?errno:0);continue;}
  fprintf(out,"FUTEX_WORD tid=%d uaddr=%#llx op=%#llx expected=%#x actual=%#x timed=%d%s\n",tid,a0,a1,(uint32_t)a2,word,a3!=0,word!=(uint32_t)a2?" CHANGED_WHILE_WAITING":"");
 }
 fclose(tasks);
}
static int ignore_xerror(Display *d,XErrorEvent *e){(void)d;(void)e;return 0;}
static void x11_pixels(FILE *out){
 Display *d=XOpenDisplay(NULL);if(!d){fputs("X11_PIXELS connection=unavailable\n",out);return;}
 XSetErrorHandler(ignore_xerror);int opcode,event,error;
 fprintf(out,"X11_PIXELS connection=PASS XDamage_advertised=%d events=not_sampled\n",XQueryExtension(d,"DAMAGE",&opcode,&event,&error));
 Window root,parent,*children=NULL;unsigned count=0;
 if(XQueryTree(d,DefaultRootWindow(d),&root,&parent,&children,&count))for(unsigned i=0;i<count&&i<64;i++){
  XWindowAttributes a;if(!XGetWindowAttributes(d,children[i],&a)||a.map_state!=IsViewable||a.width<64||a.height<64)continue;
  unsigned w=a.width<128?(unsigned)a.width:128,h=a.height<128?(unsigned)a.height:128;
  XImage *im=XGetImage(d,children[i],(a.width-w)/2,(a.height-h)/2,w,h,AllPlanes,ZPixmap);if(!im)continue;
  unsigned nonblack=0,different=0;unsigned long first=XGetPixel(im,0,0);unsigned long long hash=1469598103934665603ULL;
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){unsigned long v=XGetPixel(im,x,y);nonblack+=v!=0;different+=v!=first;hash^=v;hash*=1099511628211ULL;}
  fprintf(out,"X11_WINDOW_PIXELS window=%lx size=%ux%u nonblack=%u different_from_first=%u hash=%llx application_content=UNVERIFIED\n",children[i],w,h,nonblack,different,hash);XDestroyImage(im);
 }
 if(children)XFree(children);
 XCloseDisplay(d);
}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 FILE *out=fopen(argv[1],"a");if(!out)return 1;
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);fprintf(out,"\nRUNTIME_SNAPSHOT monotonic=%lld.%09ld\n",(long long)ts.tv_sec,ts.tv_nsec);
 x11_pixels(out);
 copy(out,"/proc/ish/tasks",1024*1024);poll_wait_arguments(out);futex_wait_words(out);DIR *proc=opendir("/proc");if(!proc){fclose(out);return 1;}
 struct dirent *e;unsigned count=0;
 while((e=readdir(proc))&&count<64){char *end;long pid=strtol(e->d_name,&end,10);if(*end||pid<=0)continue;
  char path[256],line[512];snprintf(path,sizeof path,"/proc/%ld/status",pid);FILE *status=fopen(path,"r");if(!status)continue;
  long tgid=0;while(fgets(line,sizeof line,status))if(sscanf(line,"Tgid: %ld",&tgid)==1)break;fclose(status);if(tgid!=pid)continue;count++;
  fprintf(out,"\nPROCESS pid=%ld exe=",pid);snprintf(path,sizeof path,"/proc/%ld/exe",pid);ssize_t n=readlink(path,line,sizeof line-1);if(n>=0){line[n]=0;fputs(line,out);}fputc('\n',out);
  snprintf(path,sizeof path,"/proc/%ld/cmdline",pid);FILE *cmd=fopen(path,"r");
  if(cmd){char args[4096];size_t len=fread(args,1,sizeof args-1,cmd);args[len]=0;fclose(cmd);
   for(size_t i=0;i<len;){size_t size=strnlen(args+i,len-i);if(!strncmp(args+i,"--type=",7)||!strncmp(args+i,"--utility-sub-type=",19))fprintf(out,"ROLE %.*s\n",(int)size,args+i);i+=size+1;}}
  const char *files[]={"stat","status","maps"};for(unsigned i=0;i<3;i++){snprintf(path,sizeof path,"/proc/%ld/%s",pid,files[i]);fprintf(out,"%s\n",files[i]);copy(out,path,i==2?262144:4096);}
  snprintf(path,sizeof path,"/proc/%ld/fdinfo",pid);DIR *fds=opendir(path);unsigned entries=0;struct dirent *fd;
  if(fds){while((fd=readdir(fds))&&entries<64){long num=strtol(fd->d_name,&end,10);if(*end||num<0)continue;entries++;
    snprintf(path,sizeof path,"/proc/%ld/fd/%ld",pid,num);n=readlink(path,line,sizeof line-1);if(n>=0)line[n]=0;else strcpy(line,"unavailable");fprintf(out,"FD %ld target=%s ",num,line);
    snprintf(path,sizeof path,"/proc/%ld/fdinfo/%ld",pid,num);copy(out,path,256);
  }closedir(fds);}
 }
 closedir(proc);int result=ferror(out)?1:0;if(fclose(out))result=1;return result;
}
