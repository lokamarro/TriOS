#define _GNU_SOURCE
#include <pthread.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static void *wake_without_readiness(void *arg) {
 int fd=*(int*)arg;uint64_t zero=0;
 for(int i=0;i<150;i++){if(write(fd,&zero,sizeof zero)!=sizeof zero)return (void*)1;struct timespec ts={0,10000000};nanosleep(&ts,NULL);}return NULL;
}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(void){int fd=eventfd(0,EFD_CLOEXEC);if(fd<0)return 2;pthread_t writer;if(pthread_create(&writer,NULL,wake_without_readiness,&fd))return 3;
 struct pollfd p={fd,POLLIN,0};double begin=now();int result=poll(&p,1,250);double elapsed=now()-begin;
 void *status;pthread_join(writer,&status);close(fd);
 int good=result==0&&elapsed>=.20&&elapsed<.65&&status==NULL;
 printf("POLL_DEADLINE_%s result=%d elapsed=%.6f requested=0.250 spurious_wakes=150\n",good?"PASS":"FAIL",result,elapsed);return !good;}
