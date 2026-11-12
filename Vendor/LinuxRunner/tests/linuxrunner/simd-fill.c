#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
static uint32_t buf[1024];
int main(void) {
 alarm(5);
 for(uint64_t n=16;n<128;n++) {
  uint64_t start=(uintptr_t)buf,end,rem,after;
  uint32_t color=0x12345678;
  __asm__ volatile("mov x0,%[base]\n mov x1,%[color]\n mov x2,%[count]\n negs x3,x2\n and x4,x2,#15\n and x3,x3,#15\n fmov s31,w1\n csneg x3,x4,x3,mi\n mov %[rem],x3\n dup v1.4s,v31.s[0]\n sub x2,x2,x3\n add x2,x0,x2,lsl #2\n mov %[end],x2\n mov v0.16b,v1.16b\n 1: st2 {v0.4s,v1.4s},[x0],#32\n st2 {v0.4s,v1.4s},[x0],#32\n cmp x2,x0\n b.ne 1b\n mov %[after],x0\n"
  :[end]"=r"(end),[rem]"=r"(rem),[after]"=r"(after)
  :[base]"r"(start),[color]"r"((uint64_t)color),[count]"r"(n)
  :"x0","x1","x2","x3","x4","v0","v1","v31","cc","memory");
  if(rem!=n%16||end!=start+(n-n%16)*4||after!=end) {printf("FAIL n=%lu rem=%lu end_delta=%ld after_delta=%ld\n",n,rem,end-start,after-start);return 1;}
  for(int i=0;i<(n-n%16);i++)if(buf[i]!=color){printf("FAIL pixel%d=%x\n",i,buf[i]);return 1;}
 }
 puts("SIMD_FILL: PASS");
}
