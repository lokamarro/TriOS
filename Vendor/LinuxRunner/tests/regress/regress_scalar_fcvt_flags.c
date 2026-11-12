#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t digest=1469598103934665603ULL;
static unsigned cases;
static void record(uint64_t r,uint64_t flags){digest^=r;digest*=1099511628211ULL;digest^=flags;digest*=1099511628211ULL;cases++;}
#define FOUR(op) do{uint64_t r,flags; \
 __asm__ volatile("msr fpsr,%2\n\t" op " %w0,%s3\n\tmrs %1,fpsr" : "=&r"(r),"=&r"(flags) : "r"(seed),"w"(s));record(r,flags); \
 __asm__ volatile("msr fpsr,%2\n\t" op " %w0,%d3\n\tmrs %1,fpsr" : "=&r"(r),"=&r"(flags) : "r"(seed),"w"(d));record(r,flags); \
 __asm__ volatile("msr fpsr,%2\n\t" op " %0,%s3\n\tmrs %1,fpsr" : "=&r"(r),"=&r"(flags) : "r"(seed),"w"(s));record(r,flags); \
 __asm__ volatile("msr fpsr,%2\n\t" op " %0,%d3\n\tmrs %1,fpsr" : "=&r"(r),"=&r"(flags) : "r"(seed),"w"(d));record(r,flags); \
}while(0)
int main(void){
 const uint64_t bits[]={0,0x8000000000000000ULL,0x3ff0000000000000ULL,0xbff0000000000000ULL,0x3ff8000000000000ULL,0xbff8000000000000ULL,0x41e0000000000000ULL,0x41f0000000000000ULL,0xc1e0000000000000ULL,0xc1e0000000200000ULL,0x43e0000000000000ULL,0x43f0000000000000ULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000001ULL,0x7ff0000000000001ULL,1,0x8000000000000001ULL,0x0010000000000000ULL,0x8010000000000000ULL};
 uint64_t original,original_flags;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(original),"=r"(original_flags));
 for(unsigned mode=0;mode<4;mode++){
  uint64_t fpcr=((uint64_t)(mode&1)<<24)|((uint64_t)(mode>>1)<<25);__asm__ volatile("msr fpcr,%0"::"r"(fpcr));
  for(unsigned initial=0;initial<2;initial++)for(unsigned i=0;i<sizeof(bits)/sizeof(bits[0]);i++){
   double d;memcpy(&d,&bits[i],8);float s=(float)d;uint64_t seed=initial?2:0;
   FOUR("fcvtzs");FOUR("fcvtzu");
  }
 }
 __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(original),"r"(original_flags));
 printf("SCALAR_FCVT_FLAGS cases=%u digest=%016llx\n",cases,(unsigned long long)digest);return 0;
}
