/* 4 compute threads + 1 thread mapping/unmapping for 5 s: compute throughput and
 * mmap rate, the path the writer/reader cooperation changes. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static atomic_int stop; static atomic_ulong work;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void *compute(void *a){(void)a;unsigned long n=0,x=1;while(!atomic_load_explicit(&stop,memory_order_relaxed)){for(int i=0;i<1000;i++)x=x*6364136223846793005ul+1442695040888963407ul;n++;}atomic_fetch_add(&work,n+(x&1));return NULL;}
#include <stdlib.h>
int main(int argc,char**argv){int threads=argc>1?atoi(argv[1]):4,maps_on=argc>2?atoi(argv[2]):1;pthread_t t[8];for(int i=0;i<threads;i++)pthread_create(&t[i],0,compute,0);
 double t0=now();unsigned long maps=0;while(now()-t0<5){if(!maps_on){usleep(100000);continue;}char*p=mmap(0,1<<16,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);p[0]=1;munmap(p,1<<16);maps++;}
 atomic_store(&stop,1);for(int i=0;i<threads;i++)pthread_join(t[i],0);double el=now()-t0;
 printf("BENCH compute_kiter_per_s=%.0f mmap_per_s=%.0f\n",atomic_load(&work)/el,maps/el);return 0;}
