/* Test-only RNG workload. No random output is logged or substituted. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/auxv.h>
extern int RAND_bytes(unsigned char *, int);
static double now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(2);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv){int n=argc>1?atoi(argv[1]):50;if(n<1||n>1000)return 2;alarm(120);unsigned char *p=malloc(180000);if(!p)return 2;double start=now();for(int i=0;i<n;i++)if(RAND_bytes(p,180000)!=1)return 1;double elapsed=now()-start;free(p);printf("RANDOM_THROUGHPUT: hwcap=%#lx bytes=%llu calls=%d seconds=%.6f MiB_per_s=%.3f PASS\n",getauxval(AT_HWCAP),(unsigned long long)n*180000,n,elapsed,n*180000.0/1048576/elapsed);return 0;}
