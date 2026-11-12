#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static int64_t ns(struct timespec t){return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void){
 alarm(15);struct timespec before,after,res;
 assert(!clock_getres(CLOCK_THREAD_CPUTIME_ID,&res)&&ns(res)>0);
 assert(!clock_getres(CLOCK_THREAD_CPUTIME_ID,NULL));
 assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&before));
 volatile unsigned value=1;for(unsigned i=0;i<200000;i++)value=value*1664525+1013904223;
 assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&after)&&ns(after)>ns(before));
 printf("THREAD_CLOCK_PASS cpu_ns=%lld resolution_ns=%lld value=%u\n",(long long)(ns(after)-ns(before)),(long long)ns(res),value);
 errno=0;assert(clock_gettime((clockid_t)999,&after)==-1&&errno==EINVAL);
 errno=0;assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID,(struct timespec*)1)==-1&&errno==EFAULT);
 assert(!clock_gettime(CLOCK_MONOTONIC,&after));assert(!clock_gettime(CLOCK_REALTIME,&after));
}
