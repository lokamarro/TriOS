#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
/* Bounded guest workloads. Same executable, counts and checksum in A/B runs.
 * Volatile data prevents optimizing away memory/branch work. No timed logging.
 */
static volatile uint64_t data[4096];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t shared;
static void *worker(void *arg) {
 unsigned n=*(unsigned*)arg;
 for(unsigned i=0;i<n;i++){pthread_mutex_lock(&lock);shared++;pthread_mutex_unlock(&lock);}return NULL;
}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 unsigned n=strtoul(argv[2],NULL,10);uint64_t sum=0;
 if(!strcmp(argv[1],"memory")) {
  for(unsigned i=0;i<4096;i++)data[i]=i;
  for(unsigned i=0;i<n;i++){unsigned j=i&4095;data[j]=data[j]*1664525+1013904223;sum^=data[j];}
 } else if(!strcmp(argv[1],"integer")) {
  uint64_t x=1;for(unsigned i=0;i<n;i++){x=x*UINT64_C(6364136223846793005)+1;sum^=x;} 
 } else if(!strcmp(argv[1],"branch")) {
  for(unsigned i=0;i<n;i++) {uint64_t v=data[i&4095];if(i&1)sum+=v+i;else sum^=v+i;data[i&4095]=sum;}
 } else if(!strcmp(argv[1],"float")) {
  volatile double x=1.0;for(unsigned i=0;i<n;i++)x=x*1.0000001+0.0000003;sum=(uint64_t)(x*1000000);
 } else if(!strcmp(argv[1],"syscall")) {
  struct timespec t;for(unsigned i=0;i<n;i++){if(clock_gettime(CLOCK_MONOTONIC,&t))return 3;sum++;}
 } else if(!strcmp(argv[1],"threads")) {
  pthread_t threads[4];for(unsigned i=0;i<4;i++)if(pthread_create(&threads[i],NULL,worker,&n))return 4;
  for(unsigned i=0;i<4;i++)if(pthread_join(threads[i],NULL))return 5;
  sum=shared;if(sum!=(uint64_t)n*4)return 6;
 } else return 7;
 printf("%s %llu\n",argv[1],(unsigned long long)sum);return 0;
}
