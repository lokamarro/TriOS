#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static _Atomic unsigned word, entered, count, signal_seen;
static unsigned tid(void){return syscall(SYS_gettid);}
static long op(_Atomic unsigned *w,int cmd,const struct timespec *t){return syscall(SYS_futex,w,cmd|FUTEX_PRIVATE_FLAG,0,t,0,0);}
static void pause_ms(int ms){struct timespec t={ms/1000,(ms%1000)*1000000};nanosleep(&t,0);}
static void wait_for_waiter(void){for(int i=0;i<2000;i++){if(atomic_load(&word)&FUTEX_WAITERS)return;pause_ms(1);}assert(!"waiter was not registered");}
static void *counter(void *p){unsigned cycles=(unsigned)(uintptr_t)p;for(unsigned i=0;i<cycles;i++){assert(!op(&word,FUTEX_LOCK_PI,0));assert((word&FUTEX_TID_MASK)==tid());unsigned n=count;pause_ms(i%251==0?1:0);count=n+1;assert(!op(&word,FUTEX_UNLOCK_PI,0));}return 0;}
static void *timed(void *p){(void)p;struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_nsec+=100000000;if(end.tv_nsec>=1000000000){end.tv_sec++;end.tv_nsec-=1000000000;}errno=0;assert(op(&word,FUTEX_LOCK_PI,&end)==-1&&errno==ETIMEDOUT);return 0;}
static void handler(int sig){(void)sig;signal_seen=1;}
static void *interrupted(void*p){(void)p;entered=1;struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec++;errno=0;assert(op(&word,FUTEX_LOCK_PI,&end)==-1&&errno==ETIMEDOUT);return 0;}
static void *owner_die(void*p){pthread_mutex_t *m=p;assert(!pthread_mutex_lock(m));return 0;}
static _Atomic unsigned nested_a,nested_b, nested_ready;
static void *nested(void*p){(void)p;assert(!op(&nested_b,FUTEX_LOCK_PI,0));nested_ready=1;assert(!op(&nested_a,FUTEX_LOCK_PI,0));assert(!op(&nested_a,FUTEX_UNLOCK_PI,0));assert(!op(&nested_b,FUTEX_UNLOCK_PI,0));return 0;}
int main(void){alarm(50);pthread_t t[6];
 assert(!op(&word,FUTEX_LOCK_PI,0));assert(!pthread_create(&t[0],0,counter,(void*)1));wait_for_waiter();assert(!op(&word,FUTEX_UNLOCK_PI,0));assert(!pthread_join(t[0],0));assert(count==1);puts("PI handoff PASS");
 assert(!op(&word,FUTEX_LOCK_PI,0));assert(!pthread_create(&t[0],0,timed,0));assert(!pthread_join(t[0],0));assert((word&FUTEX_TID_MASK)==tid());assert(!op(&word,FUTEX_UNLOCK_PI,0));puts("PI absolute realtime timeout PASS");
 struct sigaction sa={.sa_handler=handler};sigemptyset(&sa.sa_mask);assert(!sigaction(SIGUSR1,&sa,0));assert(!op(&word,FUTEX_LOCK_PI,0));assert(!pthread_create(&t[0],0,interrupted,0));wait_for_waiter();assert(!pthread_kill(t[0],SIGUSR1));assert(!pthread_join(t[0],0));assert(signal_seen);assert(!op(&word,FUTEX_UNLOCK_PI,0));puts("PI signal delivery and unconditional restart PASS");
 assert(!op(&nested_a,FUTEX_LOCK_PI,0));assert(!pthread_create(&t[0],0,nested,0));while(!nested_ready)pause_ms(1);pause_ms(20);errno=0;assert(op(&nested_b,FUTEX_LOCK_PI,0)==-1&&errno==EDEADLK);assert(!op(&nested_a,FUTEX_UNLOCK_PI,0));assert(!pthread_join(t[0],0));puts("PI nested cycle detection PASS");
 pthread_mutexattr_t a;pthread_mutex_t m;assert(!pthread_mutexattr_init(&a));assert(!pthread_mutexattr_setprotocol(&a,PTHREAD_PRIO_INHERIT));assert(!pthread_mutexattr_setrobust(&a,PTHREAD_MUTEX_ROBUST));assert(!pthread_mutex_init(&m,&a));assert(!pthread_create(&t[0],0,owner_die,&m));assert(!pthread_join(t[0],0));assert(pthread_mutex_lock(&m)==EOWNERDEAD);assert(!pthread_mutex_consistent(&m));assert(!pthread_mutex_unlock(&m));assert(!pthread_mutex_destroy(&m));puts("PI robust owner death PASS");
 for(int i=0;i<6;i++)assert(!pthread_create(&t[i],0,counter,(void*)500));for(int i=0;i<6;i++)assert(!pthread_join(t[i],0));assert(count==3001);assert(!word);puts("PI 3000 contended cycles PASS");
 _Atomic unsigned *shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(shared!=MAP_FAILED);assert(!syscall(SYS_futex,shared,FUTEX_LOCK_PI,0,0,0,0));pid_t child=fork();assert(child>=0);if(!child){assert(!syscall(SYS_futex,shared,FUTEX_LOCK_PI,0,0,0,0));assert((*shared&FUTEX_TID_MASK)==tid());assert(!syscall(SYS_futex,shared,FUTEX_UNLOCK_PI,0,0,0,0));_exit(0);}pause_ms(100);assert(*shared&FUTEX_WAITERS);assert(!syscall(SYS_futex,shared,FUTEX_UNLOCK_PI,0,0,0,0));int status;assert(waitpid(child,&status,0)==child);assert(status==0);assert(!*shared);puts("PI fork shared mapping PASS");
 puts("FUTEX_PI_CONTENDED_PASS; realtime priority guarantees require separate host scheduler validation");}
