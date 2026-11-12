#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <unistd.h>
int main(void){
 uintptr_t initial=syscall(SYS_brk,0),end=((initial+4095)&~(uintptr_t)4095)+2*1024*1024;
 struct sysinfo before,grown,restored;sysinfo(&before);
 if((uintptr_t)syscall(SYS_brk,end)!=end)return 1;
 sysinfo(&grown);
 if((uintptr_t)syscall(SYS_brk,initial)!=initial)return 2;
 sysinfo(&restored);
 unsigned long expected=end-((initial+4095)&~(uintptr_t)4095);
 if(before.freeram-grown.freeram!=expected||before.freeram!=restored.freeram){
  fprintf(stderr,"BRK accounting FAIL before=%lu grown=%lu restored=%lu expected-charge=%lu\n",before.freeram,grown.freeram,restored.freeram,expected);return 3;
 }
 puts("BRK_ACCOUNTING_PASS grow charged once, shrink restores budget");return 0;
}
