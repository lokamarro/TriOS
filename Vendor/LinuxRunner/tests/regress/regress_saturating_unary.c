#include <stdint.h>
#include <stdio.h>
static uint64_t digest=1469598103934665603ULL;static unsigned count;
static void mix(uint64_t a){digest^=a;digest*=1099511628211ULL;}
#define CHECK(OP,ARR) do { for(unsigned alias=0;alias<2;alias++){uint64_t out[2],status; if(alias){__asm__ volatile("msr fpsr,%3\n\tldr q0,[%2]\n\t" #OP " v0." #ARR ",v0." #ARR "\n\tstr q0,[%1]\n\tmrs %0,fpsr":"=r"(status):"r"(out),"r"(inputs[i]),"r"(sticky):"v0","memory");}else{__asm__ volatile("msr fpsr,%3\n\tldr q1,[%2]\n\tmovi v0.16b,#255\n\t" #OP " v0." #ARR ",v1." #ARR "\n\tstr q0,[%1]\n\tmrs %0,fpsr":"=r"(status):"r"(out),"r"(inputs[i]),"r"(sticky):"v0","v1","memory");}mix(out[0]);mix(out[1]);mix(status);count++;}}while(0)
int main(void){uint64_t saved;__asm__ volatile("mrs %0,fpsr":"=r"(saved));const uint64_t inputs[][2]={{0,0},{~0ULL,~0ULL},{0x8080808080808080ULL,0x8000800080008000ULL},{0x8000000080000000ULL,0x8000000000000000ULL},{0x7fffffff7fffffffULL,0x7fffffffffffffffULL},{1,0x8000000000000001ULL}};
for(unsigned s=0;s<2;s++){uint64_t sticky=s?0x08000011:0;for(unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);i++){CHECK(sqneg,8b);CHECK(sqneg,16b);CHECK(sqneg,4h);CHECK(sqneg,8h);CHECK(sqneg,2s);CHECK(sqneg,4s);CHECK(sqneg,2d);CHECK(sqabs,8b);CHECK(sqabs,16b);CHECK(sqabs,4h);CHECK(sqabs,8h);CHECK(sqabs,2s);CHECK(sqabs,4s);CHECK(sqabs,2d);}}
__asm__ volatile("msr fpsr,%0"::"r"(saved));printf("SAT_UNARY_PASS cases=%u digest=%016llx\n",count,(unsigned long long)digest);}
