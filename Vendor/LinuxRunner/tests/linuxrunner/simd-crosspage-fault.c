#define _GNU_SOURCE
#include <sys/mman.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
static unsigned char *missing;
static uintptr_t expected_pc;
static volatile sig_atomic_t seen,wrong;
static void fault(int sig,siginfo_t *info,void *v) {
 (void)sig;ucontext_t *u=v;
 if(info->si_addr!=missing||u->uc_mcontext.pc!=expected_pc)wrong=1;
 seen++;
 if(mmap(missing,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=missing)_exit(5);
}
int main(void){alarm(5);unsigned char *a=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(a==MAP_FAILED)return 2;missing=a+4096;if(munmap(missing,4096))return 3;
 struct sigaction sa={.sa_sigaction=fault,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);sigaction(SIGSEGV,&sa,0);
 unsigned char *cursor=a+4096-48;
 __asm__ volatile("adr x6,1f\n str x6,[%[pc]]\n movi v0.4s,#42\n mov v1.16b,v0.16b\n st2 {v0.4s,v1.4s},[%[p]],#32\n 1: st2 {v0.4s,v1.4s},[%[p]],#32\n" :[p]"+r"(cursor):[pc]"r"(&expected_pc):"x6","v0","v1","memory");
 if(seen!=1||wrong||cursor!=a+4096+16){printf("FAIL seen=%d wrong=%d offset=%ld\n",seen,wrong,cursor-a);return 4;}
 uint32_t *pixels=(uint32_t*)(a+4096-48);for(int i=0;i<16;i++)if(pixels[i]!=42)return 6;
 munmap(a,12288);puts("SIMD_CROSSPAGE_FAULT:PASS actual fault address, precise PC, single writeback");}
