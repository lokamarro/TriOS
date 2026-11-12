/* Run this identical source on native ARM64 and under LinuxRunner; compare
 * the digest. Capture Xd after a Wd write to check architectural zero extension. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t digest=1469598103934665603ULL;
static unsigned cases;
static void record(uint64_t result){digest^=result;digest*=1099511628211ULL;cases++;}
#define FOUR(op,scale) do { \
 uint64_t r; \
 __asm__ volatile(op " %w0, %s1" scale : "=r"(r) : "w"(s));record(r); \
 __asm__ volatile(op " %w0, %d1" scale : "=r"(r) : "w"(d));record(r); \
 __asm__ volatile(op " %0, %s1" scale : "=r"(r) : "w"(s));record(r); \
 __asm__ volatile(op " %0, %d1" scale : "=r"(r) : "w"(d));record(r); \
} while(0)
int main(void){
 const uint64_t bits[]={
  0,0x8000000000000000ULL,0x3ff0000000000000ULL,0xbff0000000000000ULL,
  0x4000000000000000ULL,0xc000000000000000ULL,0x3ff8000000000000ULL,0xbff8000000000000ULL,
  0x41dfffffffc00000ULL,0x41e0000000000000ULL,0xc1e0000000000000ULL,0xc1e0000000200000ULL,
  0x41efffffffe00000ULL,0x41f0000000000000ULL,0x43dfffffffffffffULL,0x43e0000000000000ULL,
  0xc3e0000000000000ULL,0x43efffffffffffffULL,0x43f0000000000000ULL,
  0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000001ULL,
  0x7ff0000000000001ULL,0xfff8000000000001ULL,0x0000000000000001ULL,
  0x8000000000000001ULL,0x0010000000000000ULL,0x8010000000000000ULL,
  0x3fefffffffffffffULL,0xbfefffffffffffffULL,0x3ff0000000000001ULL,0xbff0000000000001ULL
 };
 for(unsigned i=0;i<sizeof(bits)/sizeof(bits[0]);i++){
  double d;memcpy(&d,&bits[i],8);float s=(float)d;
  FOUR("fcvtzs","");FOUR("fcvtzu","");
  FOUR("fcvtzs",", #1");FOUR("fcvtzu",", #1");
  FOUR("fcvtzs",", #16");FOUR("fcvtzu",", #16");
  FOUR("fcvtzs",", #32");FOUR("fcvtzu",", #32");
 }
 double negative=-1.0;uint64_t value;
 __asm__ volatile("fcvtzs %w0, %d1" : "=r"(value) : "w"(negative));
 if(value!=0xffffffffULL){fprintf(stderr,"FAIL FCVTZS Wd zero extension: %016llx\n",(unsigned long long)value);return 1;}
 if((value|0xfff8800000000000ULL)!=0xfff88000ffffffffULL)return 2;
 printf("SCALAR_FCVT cases=%u digest=%016llx negative-W=%016llx\n",cases,(unsigned long long)digest,(unsigned long long)value);
 return 0;
}
