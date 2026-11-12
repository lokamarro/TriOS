#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdlib.h>
static pid_t process, parent;
static void check_proc(void) {
 char path[128], line[256];
 snprintf(path,sizeof(path),"/proc/self/task/%ld/status",syscall(SYS_gettid));
 FILE *f=fopen(path,"r"); assert(f); int ppid=-1;
 while(fgets(line,sizeof(line),f)) if(sscanf(line,"PPid: %d",&ppid)==1) break;
 fclose(f); assert(ppid==parent);
}
static void *worker(void *arg) {
 assert(getppid()==parent); check_proc();
 if(!arg) { pthread_t nested; assert(!pthread_create(&nested,0,worker,(void*)1)); assert(!pthread_join(nested,0)); }
 pid_t child=fork(); assert(child>=0);
 if(!child) _exit(getppid()==process?0:1);
 int status; assert(waitpid(child,&status,0)==child); assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 return 0;
}
int main(void) {
 alarm(30); process=getpid(); parent=getppid(); check_proc();
 pthread_t t; assert(!pthread_create(&t,0,worker,0)); assert(!pthread_join(t,0));
 printf("PARENT_PROCESS_PASS pid=%d ppid=%d nested_threads fork_from_thread proc_status\n",process,parent);
}
