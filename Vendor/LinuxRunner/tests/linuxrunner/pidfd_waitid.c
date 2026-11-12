#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
static void expect(int ok,const char *s) { if(!ok){fprintf(stderr,"FAIL %s errno=%d\n",s,errno);_exit(1);} puts(s); }
static long wid(int fd,siginfo_t *info,int opts) { return syscall(SYS_waitid,P_PIDFD,fd,info,opts,NULL); }
static atomic_int created, release_worker;
static void *sibling_child(void *unused){
 pid_t child=fork();assert(child>=0);if(!child)_exit(23);
 atomic_store(&created,child);while(!atomic_load(&release_worker))usleep(1000);return NULL;
}
int main(void) {
 alarm(20); int gate[2];assert(pipe(gate)==0); pid_t pid=fork();assert(pid>=0);
 if(!pid){close(gate[1]);char c;read(gate[0],&c,1);_exit(37);}
 close(gate[0]);int fd=syscall(SYS_pidfd_open,pid,0);expect(fd>=0,"pidfd_open live child");
 expect(fcntl(fd,F_GETFD)&FD_CLOEXEC,"pidfd CLOEXEC");
 siginfo_t info;memset(&info,0xaa,sizeof info);
 expect(wid(fd,&info,WEXITED|WNOHANG)==0 && info.si_pid==0 && info.si_signo==0,"WNOHANG clears siginfo");
 errno=0;expect(wid(fd,&info,0)==-1&&errno==EINVAL,"waitid requires event option");
 int bad=open("/dev/null",O_RDONLY);errno=0;expect(wid(bad,&info,WEXITED|WNOHANG)==-1&&errno==EBADF,"non-pidfd rejected");close(bad);
 int nb=syscall(SYS_pidfd_open,pid,O_NONBLOCK);expect(nb>=0,"nonblocking pidfd");
 errno=0;expect(wid(nb,&info,WEXITED)==-1&&errno==EAGAIN,"nonblocking wait EAGAIN");
 close(gate[1]);struct pollfd p={fd,POLLIN,0};expect(poll(&p,1,5000)==1&&(p.revents&POLLIN),"exit readiness");
 // Opening a zombie must initialize readiness, not lose an already-fired wake.
 int zombie=syscall(SYS_pidfd_open,pid,0);expect(zombie>=0,"pidfd_open zombie");
 struct pollfd z={zombie,POLLIN,0};expect(poll(&z,1,0)==1,"zombie immediately readable");
 expect(wid(fd,&info,WEXITED|WNOWAIT)==0&&info.si_pid==pid&&info.si_code==CLD_EXITED&&info.si_status==37,"WNOWAIT exit status");
 expect(wid(fd,&info,WEXITED)==0&&info.si_status==37,"reap via pidfd");
 errno=0;expect(wid(fd,&info,WEXITED|WNOHANG)==-1&&errno==ECHILD,"reaped child ECHILD");
 close(fd);close(nb);close(zombie);
 int self=syscall(SYS_pidfd_open,getpid(),0);errno=0;expect(wid(self,&info,WEXITED|WNOHANG)==-1&&errno==ECHILD,"nonchild ECHILD");close(self);
 for(int i=0;i<80;i++){pid=fork();assert(pid>=0);if(!pid)_exit(i);fd=syscall(SYS_pidfd_open,pid,0);expect(fd>=0,"racing open");expect(wid(fd,&info,WEXITED)==0&&info.si_pid==pid&&info.si_status==i,"race exit/reap");close(fd);}
 pthread_t worker;expect(pthread_create(&worker,NULL,sibling_child,NULL)==0,"sibling worker");
 while(!atomic_load(&created))usleep(1000);
 expect(waitid(P_ALL,0,&info,WEXITED)==0&&info.si_pid==atomic_load(&created)&&info.si_status==23,"wait child created by sibling thread");
 atomic_store(&release_worker,1);pthread_join(worker,NULL);
 puts("PIDFD_WAITID_PASS");return 0;
}
