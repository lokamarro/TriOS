#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static int word;
static atomic_int entered, finished;
static double seconds(struct timespec t) { return t.tv_sec+t.tv_nsec/1e9; }
static struct timespec deadline(clockid_t clock) { struct timespec t;assert(!clock_gettime(clock,&t));t.tv_nsec+=100000000;if(t.tv_nsec>=1000000000){t.tv_sec++;t.tv_nsec-=1000000000;}return t; }
static void timed(clockid_t clock,int flag) {
 struct timespec start,end,until=deadline(clock);clock_gettime(CLOCK_MONOTONIC,&start);
 errno=0;assert(syscall(SYS_futex,&word,FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG|flag,0,&until,0,FUTEX_BITSET_MATCH_ANY)==-1&&errno==ETIMEDOUT);
 clock_gettime(CLOCK_MONOTONIC,&end);double elapsed=seconds(end)-seconds(start);assert(elapsed>=.09&&elapsed<2);
 printf("FUTEX_TIMEOUT clock=%d elapsed=%.6f\n",clock,elapsed);
}
static void *waiter(void *arg) {
 unsigned mask=(unsigned)(unsigned long)arg; atomic_fetch_add(&entered,1);
 struct timespec until=deadline(CLOCK_MONOTONIC);until.tv_sec+=3;
 assert(syscall(SYS_futex,&word,FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG,0,&until,0,mask)==0);
 atomic_fetch_or(&finished,mask);return 0;
}
int main(void) {
 alarm(20); timed(CLOCK_MONOTONIC,0);timed(CLOCK_REALTIME,FUTEX_CLOCK_REALTIME);
 struct timespec t={0,1000000000};errno=0;
 assert(syscall(SYS_futex,&word,FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG,0,&t,0,1)==-1&&errno==EINVAL);
 t=(struct timespec){0,0};errno=0;
 assert(syscall(SYS_futex,&word,FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG,0,&t,0,1)==-1&&errno==ETIMEDOUT);
 errno=0;assert(syscall(SYS_futex,&word,FUTEX_WAKE_BITSET|FUTEX_PRIVATE_FLAG,1,0,0,0)==-1&&errno==EINVAL);
 errno=0;assert(syscall(SYS_futex,&word,FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG,0,0,0,0)==-1&&errno==EINVAL);
 pthread_t a,b;assert(!pthread_create(&a,0,waiter,(void*)1));assert(!pthread_create(&b,0,waiter,(void*)2));
 while(atomic_load(&entered)<2)usleep(1000);usleep(50000);
 assert(syscall(SYS_futex,&word,FUTEX_WAKE_BITSET|FUTEX_PRIVATE_FLAG,2,0,0,4)==0);
 assert(syscall(SYS_futex,&word,FUTEX_WAKE_BITSET|FUTEX_PRIVATE_FLAG,2,0,0,1)==1);
 assert(!pthread_join(a,0));assert(atomic_load(&finished)==1);
 assert(syscall(SYS_futex,&word,FUTEX_WAKE|FUTEX_PRIVATE_FLAG,1,0,0,0)==1);
 assert(!pthread_join(b,0));assert(atomic_load(&finished)==3);
 puts("FUTEX_BITSET_PASS monotonic realtime expiry validation selective_wake ordinary_wake");
}
