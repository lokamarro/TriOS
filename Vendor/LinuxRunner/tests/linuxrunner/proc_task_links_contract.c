#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static atomic_int ready,release_threads;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"PROC_TASK_LINKS_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void links(unsigned n){struct stat s;int p=open("/proc",O_RDONLY|O_DIRECTORY);CHECK(p>=0);CHECK(fstatat(p,"self/task/",&s,0)==0);CHECK(S_ISDIR(s.st_mode)&&s.st_nlink==n);CHECK(close(p)==0);}
static void *thread_main(void *arg){(void)arg;atomic_fetch_add(&ready,1);while(!atomic_load(&release_threads))usleep(1000);return NULL;}
int main(void){
 links(3);pthread_t a,b;CHECK(pthread_create(&a,NULL,thread_main,NULL)==0&&pthread_create(&b,NULL,thread_main,NULL)==0);
 while(atomic_load(&ready)!=2)usleep(1000);links(5);
 pid_t child=fork();CHECK(child>=0);if(!child){links(3);_exit(0);}int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);links(5);
 atomic_store(&release_threads,1);CHECK(pthread_join(a,NULL)==0&&pthread_join(b,NULL)==0);links(3);
 struct stat s;CHECK(stat("/proc/cpuinfo",&s)==0&&s.st_nlink==1);
 printf("PROC_TASK_LINKS_PASS checks=%u single=3 two_workers=5 fork_child=3 joined=3\n",checks);return 0;
}
