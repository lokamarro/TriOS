#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef FUTEX_LOCK_PI2
#define FUTEX_LOCK_PI2 13
#endif
static _Atomic unsigned word, acquired, timedout, ready;
static unsigned protected_count;
static void ms(int n){struct timespec t={n/1000,n%1000*1000000};nanosleep(&t,0);}
static long op(unsigned *w,int operation,struct timespec *t){return syscall(SYS_futex,w,operation,0,t,0,0);}
static struct timespec end(int ms){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);t.tv_nsec+=ms*1000000;if(t.tv_nsec>=1000000000){t.tv_sec++;t.tv_nsec-=1000000000;}return t;}
static void *race(void*p){(void)p;int rounds=getenv("LR_PI_STRESS_ROUNDS")?atoi(getenv("LR_PI_STRESS_ROUNDS")):1;assert(rounds>=1&&rounds<=40);for(int i=0;i<700*rounds;i++){struct timespec t=end(2);int r=op((unsigned*)&word,FUTEX_LOCK_PI2|FUTEX_PRIVATE_FLAG,&t);if(!r){assert((word&FUTEX_TID_MASK)==syscall(SYS_gettid));protected_count++;acquired++;if(i%19==0)ms(3);assert(!op((unsigned*)&word,FUTEX_UNLOCK_PI|FUTEX_PRIVATE_FLAG,0));}else{assert(errno==ETIMEDOUT);timedout++;}}return 0;}
static unsigned *shared;
static void *wait_for_killed_owner(void*p){(void)p;assert(!op(shared,FUTEX_LOCK_PI,0));assert((*shared&FUTEX_TID_MASK)==syscall(SYS_gettid));assert(*shared&FUTEX_OWNER_DIED);assert(!op(shared,FUTEX_UNLOCK_PI,0));return 0;}
int main(void){alarm(90);struct timespec start;clock_gettime(CLOCK_MONOTONIC,&start);pthread_t threads[8];for(int i=0;i<8;i++)assert(!pthread_create(&threads[i],0,race,0));for(int i=0;i<8;i++)assert(!pthread_join(threads[i],0));assert(acquired==protected_count);int rounds=getenv("LR_PI_STRESS_ROUNDS")?atoi(getenv("LR_PI_STRESS_ROUNDS")):1;assert(acquired+timedout==5600*rounds&&timedout>0);assert(!word);struct timespec finish;clock_gettime(CLOCK_MONOTONIC,&finish);printf("PI timed handoff race PASS acquired=%u timeouts=%u seconds=%.3f\n",acquired,timedout,finish.tv_sec-start.tv_sec+(finish.tv_nsec-start.tv_nsec)/1e9);
 shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(shared!=MAP_FAILED);pid_t child=fork();assert(child>=0);if(!child){assert(!op(shared,FUTEX_LOCK_PI,0));while(1)ms(10);}
 for(int i=0;!(*shared&FUTEX_TID_MASK)&&i<1000;i++)ms(1);assert(*shared&FUTEX_TID_MASK);assert(!pthread_create(&threads[0],0,wait_for_killed_owner,0));for(int i=0;!(*shared&FUTEX_WAITERS)&&i<1000;i++)ms(1);assert(*shared&FUTEX_WAITERS);assert(!kill(child,SIGKILL));assert(!pthread_join(threads[0],0));int status;assert(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);assert(!*shared);munmap(shared,4096);puts("PI killed owner across fork PASS");
 shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(shared!=MAP_FAILED);assert(!op(shared,FUTEX_LOCK_PI,0));child=fork();assert(child>=0);if(!child){struct timespec t=end(300);errno=0;assert(op(shared,FUTEX_LOCK_PI2,&t)==-1&&errno==ETIMEDOUT);_exit(0);}for(int i=0;!(*shared&FUTEX_WAITERS)&&i<1000;i++)ms(1);assert(*shared&FUTEX_WAITERS);assert(!kill(child,SIGSTOP));ms(100);assert(!kill(child,SIGCONT));assert(waitpid(child,&status,0)==child&&status==0);assert(!op(shared,FUTEX_UNLOCK_PI,0));munmap(shared,4096);puts("PI stop/continue absolute deadline restart PASS");puts("FUTEX_PI_STRESS_PASS");}
