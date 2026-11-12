#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
static volatile sig_atomic_t handled;
static void trap(int sig,siginfo_t *info,void *context){(void)sig;(void)info;ucontext_t *u=context;u->uc_mcontext.pc+=4;handled++;}
static void requeue(int sig,siginfo_t *info,void *context){(void)context;struct sigaction a={.sa_handler=SIG_DFL};sigemptyset(&a.sa_mask);if(sigaction(sig,&a,0)||syscall(SYS_rt_tgsigqueueinfo,getpid(),syscall(SYS_gettid),sig,info))_exit(191);}
static void *exit_worker(void *p){(void)p;usleep(20000);write(2,"FATAL test worker exit 191\n",27);syscall(SYS_exit_group,191);return 0;}
int main(int argc,char **argv){if(argc!=2)return 2;
 if(!strcmp(argv[1],"crashpad")){struct sigaction a={.sa_sigaction=requeue,.sa_flags=SA_SIGINFO};sigemptyset(&a.sa_mask);if(sigaction(SIGTRAP,&a,0))return 8;write(2,"FATAL Crashpad requeue contract\n",31);__asm__ volatile("brk #0");return 9;}
 if(!strcmp(argv[1],"handled")){struct sigaction a={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};sigemptyset(&a.sa_mask);if(sigaction(SIGTRAP,&a,0))return 3;__asm__ volatile("brk #0");return handled==1?0:4;}
 if(!strcmp(argv[1],"trap"))__asm__ volatile("brk #0");
 if(!strcmp(argv[1],"reserved"))__asm__ volatile(".inst 0x0ee07800");
 if(!strcmp(argv[1],"abort"))abort();
 if(!strcmp(argv[1],"segv")){*(volatile int *)0x1234=1;return 5;}
 if(!strcmp(argv[1],"exit"))return 7;
 if(!strcmp(argv[1],"worker191")){pthread_t thread;if(pthread_create(&thread,0,exit_worker,0))return 8;for(;;)pause();}
 if(!strcmp(argv[1],"output")){write(1,"WARNING stdout tail\n",20);write(2,"FATAL partial ",14);write(2,"message\n",8);return 19;}
 return 6;}
