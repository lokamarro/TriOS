#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
static pthread_t initial_thread;
static stack_t initial_stack;
static int audit_stack, stack_checked;
static void check_host_stack(void) {
 if (!audit_stack || !pthread_equal(pthread_self(), initial_thread) || stack_checked) return;
 stack_t observed;
 if (sigaltstack(NULL, &observed) || (observed.ss_flags & SS_DISABLE) ||
     observed.ss_sp != initial_stack.ss_sp || observed.ss_size != initial_stack.ss_size) {
  fprintf(stderr,"CRASH_AUDIT_HOST_ALTSTACK_REPLACED\n");_exit(95);
 }
 stack_checked=1;fprintf(stderr,"CRASH_AUDIT_HOST_ALTSTACK_PRESERVED\n");
}
static void terminated(int tid,int signal,int code){fprintf(stderr,"CRASH_AUDIT_TERMINATED tid=%d signal=%d exit=%d\n",tid,signal,code);}
static void output(int tid,int tgid,int fd,const char *text){fprintf(stderr,"CRASH_AUDIT_OUTPUT tid=%d tgid=%d fd=%d %s\n",tid,tgid,fd,text);}
static void context(const char *line){check_host_stack();fprintf(stderr,"CRASH_AUDIT_CONTEXT %s\n",line);}
__attribute__((constructor)) static void install(void){
 if (getenv("LR_CRASH_AUDIT_ALTSTACK")) {
  initial_thread=pthread_self();
  if(sigaltstack(NULL,&initial_stack))_exit(94);
  if(initial_stack.ss_flags & SS_DISABLE){
   initial_stack=(stack_t){.ss_sp=mmap(NULL,SIGSTKSZ,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0),.ss_size=SIGSTKSZ};
   if(initial_stack.ss_sp==MAP_FAILED || sigaltstack(&initial_stack,NULL))_exit(94);
  }
  audit_stack=1;
 }
 void **end=dlsym(RTLD_DEFAULT,"runtime_guest_terminated"),**lines=dlsym(RTLD_DEFAULT,"runtime_fault_context");
 if(!end||!lines){fprintf(stderr,"CRASH_AUDIT_HOOK_MISSING\n");_exit(96);}
 void **o=dlsym(RTLD_DEFAULT,"runtime_guest_output");if(o)*o=(void *)output;
 *end=(void *)terminated;*lines=(void *)context;
}
