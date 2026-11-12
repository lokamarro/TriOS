#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
static volatile sig_atomic_t trapped;
static void trap_handler(int signal,siginfo_t *info,void *frame){
 (void)info;
 ucontext_t *context=frame;
 if(signal==SIGTRAP){trapped++;context->uc_mcontext.pc+=4;}
}
int main(void){
 struct sigaction sa={.sa_sigaction=trap_handler,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);
 if(sigaction(SIGTRAP,&sa,0))return 1;
 errno=0;if(open("/nonexistent-fault-context-probe",O_RDONLY)!=-1||errno!=ENOENT)return 2;
 __asm__ volatile("brk #7" ::: "memory");
 if(trapped!=1)return 3;
 pid_t child=fork();if(child<0)return 4;
 if(child==0){*(volatile uint64_t *)(uintptr_t)0x7ffffff00000ULL=42;_exit(5);}
 int status;if(waitpid(child,&status,0)!=child)return 6;
 if(!WIFSIGNALED(status)||WTERMSIG(status)!=SIGSEGV)return 7;
 child=fork();if(child<0)return 8;
 if(!child){
  volatile unsigned char *p=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(p==MAP_FAILED||munmap((void *)(p+4096),4096))_exit(9);
  volatile unsigned char value=p[4096];(void)value;_exit(10);
 }
 if(waitpid(child,&status,0)!=child||!WIFSIGNALED(status)||WTERMSIG(status)!=SIGSEGV)return 11;
 child=fork();if(child<0)return 12;
 if(!child){__asm__ volatile(".inst 0xffffffff");_exit(13);}
 if(waitpid(child,&status,0)!=child||!WIFSIGNALED(status)||WTERMSIG(status)!=SIGILL)return 14;
 puts("FAULT_CONTEXT_PASS BRK resumes; SIGSEGV on far/adjacent-unmapped addresses; SIGILL; parent/CLI alive");return 0;
}
