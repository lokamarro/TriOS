#define _GNU_SOURCE
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#define CHECK(x) do { if (!(x)) { perror(#x); exit(1); } } while(0)
static pthread_barrier_t ready, finish;
static pid_t worker_tid;
static void *worker(void *p) {
 worker_tid=syscall(SYS_gettid);
 char s[64]; ssize_t n=readlink("/proc/self",s,sizeof(s)-1); CHECK(n>0); s[n]=0; CHECK(atoi(s)==getpid());
 pthread_barrier_wait(&ready); pthread_barrier_wait(&finish); return NULL;
}
static int read_file(const char *p,char *buf,size_t size) { int f=open(p,O_RDONLY); CHECK(f>=0); int n=read(f,buf,size-1);CHECK(n>=0);buf[n]=0;close(f);return n; }
int main(void) {
 pthread_barrier_init(&ready,0,2); pthread_barrier_init(&finish,0,2);
 pthread_t t; CHECK(!pthread_create(&t,0,worker,0));pthread_barrier_wait(&ready);
 char p[128],b[65536];snprintf(p,sizeof(p),"/proc/self/task/%d/comm",worker_tid);read_file(p,b,sizeof(b));CHECK(strlen(b)>0);
 DIR *d=opendir("/proc/self/task");CHECK(d);int seen=0;struct dirent *e;while((e=readdir(d)))if(atoi(e->d_name)==worker_tid)seen++;closedir(d);CHECK(seen==1);
 int n=read_file("/proc/self/environ",b,sizeof(b));CHECK(memmem(b,n,"LR_PROC_TEST=present",20));
 int f=open("/tmp/lr-proc-fd",O_CREAT|O_RDWR|O_TRUNC|O_CLOEXEC,0600);CHECK(f>=0);CHECK(write(f,"abc",3)==3);snprintf(p,sizeof(p),"/proc/self/fdinfo/%d",f);read_file(p,b,sizeof(b));CHECK(strstr(b,"pos:\t3"));close(f);unlink("/tmp/lr-proc-fd");
 pthread_barrier_wait(&finish);pthread_join(t,0);
 int m=syscall(SYS_memfd_create,"sandbox-test",1);CHECK(m>=0);CHECK(!ftruncate(m,8192));int *a=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,m,0);CHECK(a!=MAP_FAILED);pid_t child=fork();CHECK(child>=0);if(!child){a[0]=42;_exit(0);}int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);CHECK(a[0]==42);munmap(a,8192);close(m);
 puts("PROCFS_THREADS_AND_MEMFD: PASS");
}
