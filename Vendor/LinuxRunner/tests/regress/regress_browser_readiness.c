#define _GNU_SOURCE
/* Diagnostic prerequisite probe, not a browser compatibility claim. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
static int failures;
static void result(const char *name,int good){printf("BROWSER_PROBE: %s %s errno=%d (%s)\n",name,good?"PASS":"FAIL",good?0:errno,good?"ok":strerror(errno));fflush(stdout);if(!good)failures++;}
static void *thread(void *p){*(int*)p=42;return NULL;}
static void mapped_child(const char *name,int fd,int flags){
 int *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,flags,fd,0);if(p==MAP_FAILED){result(name,0);return;}*p=1;
 pid_t child=fork();if(child==0){*p=73;_exit(0);}int status=0;int good=child>0&&waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&*p==73;
 if(!good&&errno==0)errno=EIO;
 result(name,good);munmap(p,4096);
}
int main(void){setbuf(stdout,NULL);alarm(20);printf("BROWSER_PROBE: diagnostic only; no browser launched, no sandbox disabled\n");
 const char *paths[]={"/proc/self/status","/proc/self/maps","/proc/self/task","/dev/shm"};for(unsigned i=0;i<4;i++){errno=0;int fd=open(paths[i],O_RDONLY);result(paths[i],fd>=0);if(fd>=0)close(fd);}
 int value=0;pthread_t t;errno=0;int rc=pthread_create(&t,NULL,thread,&value);if(!rc)rc=pthread_join(t,NULL);if(rc)errno=rc;result("pthread_create/join",!rc&&value==42);
 errno=0;void *memory=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);int good=memory!=MAP_FAILED;if(good){memset(memory,7,4096);good=mprotect(memory,4096,PROT_READ)==0&&mprotect(memory,4096,PROT_READ|PROT_WRITE)==0;munmap(memory,4096);}result("mmap/mprotect RW/R",good);
 errno=0;mapped_child("fork MAP_SHARED anonymous",-1,MAP_SHARED|MAP_ANONYMOUS);
 errno=0;int fd=memfd_create("lr-browser-probe",MFD_CLOEXEC);result("memfd_create",fd>=0);if(fd>=0){errno=0;good=ftruncate(fd,4096)==0;result("memfd ftruncate",good);if(good)mapped_child("fork memfd shared mapping",fd,MAP_SHARED);close(fd);}
 errno=0;int sockets[2];good=socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0;result("Unix socketpair",good);if(good){
 int file=open("/etc/os-release",O_RDONLY);char byte='x',control[CMSG_SPACE(sizeof(int))]={0};struct iovec iov={&byte,1};struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&file,sizeof file);
 errno=0;good=file>=0&&sendmsg(sockets[0],&msg,0)==1;if(good){memset(control,0,sizeof control);msg.msg_controllen=sizeof control;good=recvmsg(sockets[1],&msg,0)==1;c=CMSG_FIRSTHDR(&msg);good=good&&c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS;if(good){int received;memcpy(&received,CMSG_DATA(c),sizeof received);char b[16];good=read(received,b,sizeof b)>0;close(received);}}
 result("SCM_RIGHTS usable received fd",good);if(file>=0)close(file);close(sockets[0]);close(sockets[1]);}
 errno=0;fd=shm_open("/lr-browser-probe",O_CREAT|O_EXCL|O_RDWR,0600);result("POSIX shm_open",fd>=0);if(fd>=0){close(fd);shm_unlink("/lr-browser-probe");}
 errno=0;int ep=epoll_create1(EPOLL_CLOEXEC),timer=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC);good=ep>=0&&timer>=0;if(good){struct epoll_event event={.events=EPOLLIN,.data.fd=timer};struct itimerspec spec={.it_value={.tv_nsec=1000000}};good=epoll_ctl(ep,EPOLL_CTL_ADD,timer,&event)==0&&timerfd_settime(timer,0,&spec,NULL)==0&&epoll_wait(ep,&event,1,1000)==1;}result("epoll/timerfd wakeup",good);if(ep>=0)close(ep);if(timer>=0)close(timer);
 printf("BROWSER_PROBE: finished missing_or_failed=%d; physical coverage and real browser remain pending\n",failures);return failures ? 1 : 0;}
