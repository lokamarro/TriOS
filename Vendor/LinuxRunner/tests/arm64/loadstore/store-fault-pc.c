#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

/* The observable increment before STR must execute exactly once. A retry at
 * the block's beginning, incorrect ucontext PC, or incorrect write width fails.
 * Both mapped PROT_NONE/readonly and unmapped pages are tested, with cold and
 * warm translations and aligned, unaligned and page-split operands.
 */
#define STORE(name, instruction) \
 __asm__(".text\n.p2align 4\n.global " #name "\n.type " #name ", %function\n" \
 #name ":\nmov x5, #0\nmov x6, x2\nadd x5, x5, #1\n.global " #name "_pc\n" \
 #name "_pc:\n" instruction "\nstr x5, [x1]\nret\n" \
 ".size " #name ", .-" #name "\n"); \
 extern void name(void *, uint64_t *, uint64_t); extern const char name##_pc[]
STORE(store8, "strb w6, [x0, #8]");
STORE(store16, "strh w6, [x0, #8]");
STORE(store32, "str w6, [x0, #8]");
STORE(store64, "str x6, [x0, #8]");
static unsigned char *pages;
static unsigned char saved[8192];
static uintptr_t expected_pc;
static uint64_t expected_value;
static volatile sig_atomic_t faults;
static unsigned char *protected_page;
static size_t protected_length;
static void handler(int sig, siginfo_t *info, void *context) {
 (void)info; ucontext_t *uc=context;
 if(sig!=SIGSEGV || uc->uc_mcontext.pc!=expected_pc ||
    uc->uc_mcontext.regs[5]!=1 || uc->uc_mcontext.regs[6]!=expected_value) _exit(40);
 if(++faults!=1) _exit(41);
 if(mmap(protected_page,protected_length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=protected_page) _exit(42);
 memcpy(protected_page,saved+(protected_page-pages),protected_length);
}
int main(void) {
 if(sysconf(_SC_PAGESIZE)!=4096) return 10;
 pages=mmap((void*)0x40000000,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
 if(pages==MAP_FAILED) return 11;
 struct sigaction sa={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};sigemptyset(&sa.sa_mask);
 if(sigaction(SIGSEGV,&sa,NULL)) return 12;
 alarm(30);
 void (*fns[])(void *,uint64_t *,uint64_t)={store8,store16,store32,store64};
 const char *pcs[]={store8_pc,store16_pc,store32_pc,store64_pc};
 unsigned cases=0;
 for(unsigned fn=0;fn<4;fn++) for(unsigned off=0;off<3;off++)
 for(unsigned warm=0;warm<2;warm++) for(unsigned protection=0;protection<6;protection++) {
  if(protection>=3 && off!=2)continue;
  unsigned width=1u<<fn, offset=off==0?64:off==1?67:4096-(width>1?width/2:1);
  if(protection>=3 && width==1)offset=4096;
  protected_page=protection<3?pages:pages+4096;
  protected_length=protection<3?8192:4096;
  memset(pages,0xa5,8192);uint64_t out=0;
  expected_value=UINT64_C(0xfedcba9876543210);expected_pc=(uintptr_t)pcs[fn];faults=0;
  if(warm) {fns[fn](pages+offset-8,&out,expected_value);if(out!=1)return 20;memset(pages,0xa5,8192);}
  memcpy(saved,pages,8192);
  if(protection%3==0) {if(munmap(protected_page,protected_length))return 21;}
  else if(mprotect(protected_page,protected_length,protection%3==1?PROT_NONE:PROT_READ))return 22;
  fns[fn](pages+offset-8,&out,expected_value);
  unsigned char expected[8192];memcpy(expected,saved,8192);memcpy(expected+offset,&expected_value,width);
  if(faults!=1||out!=1||memcmp(pages,expected,8192)) {
   fprintf(stderr,"store-fault-pc-fail width=%u off=%u warm=%u protection=%u faults=%d out=%llu\n",width,offset,warm,protection,faults,(unsigned long long)out);return 23;
  } cases++;
 }
 if(munmap(pages,8192))return 24;
 printf("store-fault-pc-ok cases=%u\n",cases);return 0;
}
