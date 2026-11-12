#define _GNU_SOURCE
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
struct clone_args { uint64_t flags,pidfd,child_tid,parent_tid,exit_signal,stack,stack_size,tls,set_tid,set_tid_size,cgroup; };
int main(void){int unused=0x12345678;struct clone_args args={.pidfd=(uintptr_t)&unused,.exit_signal=SIGCHLD};long child=syscall(435,&args,sizeof args);if(child<0){perror("clone3 unused pidfd");return 1;}if(!child)_exit(0);int status;if(waitpid(child,&status,0)!=child||status||unused!=0x12345678)return 1;puts("PASS: clone3 ignores pidfd output pointer without CLONE_PIDFD");return 0;}
