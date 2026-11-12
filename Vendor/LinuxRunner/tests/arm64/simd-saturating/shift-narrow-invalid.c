#define _GNU_SOURCE
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
static sigjmp_buf resume;
static volatile sig_atomic_t caught;
static uint32_t expected;
static void trap(int sig, siginfo_t *info, void *raw) {
    (void)info; ucontext_t *ctx=raw;
    if(sig!=SIGILL||*(uint32_t *)ctx->uc_mcontext.pc!=expected)_exit(1);
    caught++; siglongjmp(resume,1);
}
__attribute__((noinline)) static void zero_size(void) { __asm__ volatile(".inst 0x7f009fff" ::: "v31"); }
__attribute__((noinline)) static void reserved_size(void) { __asm__ volatile(".inst 0x7f409fff" ::: "v31"); }
int main(void) {
    alarm(10); struct sigaction sa={.sa_sigaction=trap,.sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask); if(sigaction(SIGILL,&sa,NULL))return 1;
    expected=0x7f009fff; if(!sigsetjmp(resume,1)){zero_size();return 1;}
    expected=0x7f409fff; if(!sigsetjmp(resume,1)){reserved_size();return 1;}
    if(caught!=2)return 1;
    puts("SCALAR_SHIFT_RESERVED_PASS zero_immh=SIGILL immh_high=SIGILL fault_PC=PASS");return 0;
}
