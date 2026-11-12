#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static _Atomic int target_tid;
static volatile sig_atomic_t sequence,count;
static void receive(int sig,siginfo_t *info,void *unused){(void)sig;(void)unused;assert(info->si_code==SI_QUEUE&&info->si_value.sival_int==sequence);sequence++;count++;}
static void *thread_receive(void *unused){(void)unused;sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGRTMIN);assert(!pthread_sigmask(SIG_BLOCK,&mask,0));atomic_store(&target_tid,syscall(SYS_gettid));siginfo_t info;struct timespec timeout={2,0};assert(sigtimedwait(&mask,&info,&timeout)==SIGRTMIN&&info.si_value.sival_int==42);return 0;}
int main(void){_Static_assert(sizeof(siginfo_t)==128,"Linux siginfo size");sigset_t mask,old,pending;sigemptyset(&mask);sigaddset(&mask,SIGRTMIN);sigaddset(&mask,SIGRTMAX);sigaddset(&mask,SIGILL);assert(!pthread_sigmask(SIG_BLOCK,&mask,&old));siginfo_t info={0},got;info.si_code=SI_QUEUE;info.si_pid=getpid();info.si_uid=getuid();for(int i=0;i<3;i++){info.si_value.sival_int=i;assert(!syscall(SYS_rt_sigqueueinfo,getpid(),SIGRTMIN,&info));}struct timespec zero={0,0};for(int i=0;i<3;i++){memset(&got,0xaa,sizeof got);assert(sigtimedwait(&mask,&got,&zero)==SIGRTMIN&&got.si_code==SI_QUEUE&&got.si_pid==getpid()&&got.si_value.sival_int==i);assert(!sigpending(&pending));assert(!!sigismember(&pending,SIGRTMIN)==(i<2));}errno=0;assert(sigtimedwait(&mask,&got,&zero)==-1&&errno==EAGAIN);
 info.si_value.sival_int=64;assert(!syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),SIGRTMAX,&info));assert(sigtimedwait(&mask,&got,&zero)==SIGRTMAX&&got.si_value.sival_int==64);
 info=(siginfo_t){.si_code=ILL_ILLOPC};info.si_addr=(void *)0x12345678;assert(!syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),SIGILL,&info));assert(sigtimedwait(&mask,&got,&zero)==SIGILL&&got.si_code==ILL_ILLOPC&&got.si_addr==(void *)0x12345678);
 info=(siginfo_t){.si_code=SI_QUEUE,.si_pid=getpid(),.si_uid=getuid()};errno=0;assert(syscall(SYS_rt_tgsigqueueinfo,getpid()+1,syscall(SYS_gettid),SIGRTMIN,&info)==-1&&errno==ESRCH);errno=0;assert(syscall(SYS_rt_tgsigqueueinfo,getpid(),0,SIGRTMIN,&info)==-1&&errno==EINVAL);errno=0;assert(syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),SIGRTMIN,(void *)1)==-1&&errno==EFAULT);
 int pipefd[2];assert(!pipe(pipefd));pid_t child=fork();assert(child>=0);if(!child){close(pipefd[0]);write(pipefd[1],"r",1);pause();_exit(9);}close(pipefd[1]);char byte;assert(read(pipefd[0],&byte,1)==1);close(pipefd[0]);info.si_code=SI_KERNEL;errno=0;assert(syscall(SYS_rt_tgsigqueueinfo,child,child,SIGILL,&info)==-1&&errno==EPERM);assert(!kill(child,SIGKILL));int status;assert(waitpid(child,&status,0)==child&&WIFSIGNALED(status));
 struct sigaction action={.sa_sigaction=receive,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);assert(!sigaction(SIGRTMIN,&action,0));info.si_code=SI_QUEUE;for(int i=0;i<3;i++){info.si_value.sival_int=i;assert(!syscall(SYS_rt_sigqueueinfo,getpid(),SIGRTMIN,&info));}sigset_t rt;sigemptyset(&rt);sigaddset(&rt,SIGRTMIN);assert(!pthread_sigmask(SIG_UNBLOCK,&rt,0));for(int i=0;i<100&&count<3;i++)usleep(1000);assert(count==3);
 assert(!pthread_sigmask(SIG_BLOCK,&rt,0));pthread_t thread;assert(!pthread_create(&thread,0,thread_receive,0));while(!atomic_load(&target_tid))usleep(100);info.si_value.sival_int=42;assert(!syscall(SYS_rt_tgsigqueueinfo,getpid(),atomic_load(&target_tid),SIGRTMIN,&info));assert(!pthread_join(thread,0));
 int fd=signalfd(-1,&rt,SFD_CLOEXEC|SFD_NONBLOCK);assert(fd>=0);for(int i=0;i<2;i++){info.si_value.sival_int=20+i;assert(!syscall(SYS_rt_sigqueueinfo,getpid(),SIGRTMIN,&info));}for(int i=0;i<2;i++){struct signalfd_siginfo value;assert(read(fd,&value,sizeof value)==sizeof value&&value.ssi_signo==SIGRTMIN&&value.ssi_int==20+i&&value.ssi_pid==(unsigned)getpid());assert(!sigpending(&pending)&&!!sigismember(&pending,SIGRTMIN)==(i==0));}close(fd);assert(!pthread_sigmask(SIG_SETMASK,&old,0));puts("QUEUED_SIGNAL PASS self kernel-info requeue, RT64, FIFO, payloads, pending bits, targeted thread, signalfd and anti-forgery errors");return 0;}
