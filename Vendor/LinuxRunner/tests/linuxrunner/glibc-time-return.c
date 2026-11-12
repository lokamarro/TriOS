#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static void *run(void *unused) {
 (void)unused;
 for(unsigned i=0;i<1000;i++) {
  struct timespec real,coarse;time_t out=0;
  if(clock_gettime(CLOCK_REALTIME,&real)||clock_gettime(CLOCK_REALTIME_COARSE,&coarse))exit(2);
  time_t a=time(0),b=time(&out);
  if(a<=0||b!=out||llabs(a-real.tv_sec)>5||llabs(b-coarse.tv_sec)>5)exit(3);
 }
 return 0;
}
int main(void){alarm(12);pthread_t t[4];for(int i=0;i<4;i++)if(pthread_create(&t[i],0,run,0))return 4;for(int i=0;i<4;i++)pthread_join(t[i],0);puts("GLIBC_TIME_RETURN: PASS concurrent real/coarse/time calls and returns");}
