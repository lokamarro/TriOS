#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
static pthread_barrier_t ready;
static void *fill(void *unused) {
 (void)unused;
 size_t n=262144, size=16*1024*1024+4096;
 uint32_t *map=mmap(0,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
 if(map==MAP_FAILED||mprotect(map,size,PROT_READ|PROT_WRITE))exit(2);
 uint32_t *base=map+4;
 pthread_barrier_wait(&ready);
 uint32_t *cursor=base,*end=base+n;
 __asm__ volatile("movi v0.4s,#42\n mov v1.16b,v0.16b\n 1: st2 {v0.4s,v1.4s},[%[p]],#32\n st2 {v0.4s,v1.4s},[%[p]],#32\n cmp %[end],%[p]\n b.ne 1b\n"
  :[p]"+r"(cursor):[end]"r"(end):"v0","v1","cc","memory");
 if(cursor!=end)exit(3);
 for(size_t i=0;i<n;i++)if(base[i]!=42){printf("bad pixel %zu\n",i);exit(4);}
 munmap(map,size);return 0;
}
int main(void){alarm(8);pthread_barrier_init(&ready,0,3);pthread_t a,b;pthread_create(&a,0,fill,0);pthread_create(&b,0,fill,0);pthread_barrier_wait(&ready);pthread_join(a,0);pthread_join(b,0);puts("SIMD_LAZY_CONTENDED:PASS precise cross-page retry and writeback");}
