#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void check(uint64_t a,uint64_t b,int wide,int alias,uint64_t expected){
 uint64_t inputs[4]={a,~0ULL,b,~0ULL},out[2];
 if(wide){if(alias)asm volatile("ldp q0,q1,[%1]\n\tfcmeq d0,d0,d1\n\tstr q0,[%0]"::"r"(out),"r"(inputs):"v0","v1","memory");
 else asm volatile("ldp q0,q1,[%1]\n\tmovi v2.16b,#0xff\n\tfcmeq d2,d0,d1\n\tstr q2,[%0]"::"r"(out),"r"(inputs):"v0","v1","v2","memory");}
 else {if(alias)asm volatile("ldp q0,q1,[%1]\n\tfcmeq s1,s0,s1\n\tstr q1,[%0]"::"r"(out),"r"(inputs):"v0","v1","memory");
 else asm volatile("ldp q0,q1,[%1]\n\tmovi v2.16b,#0xff\n\tfcmeq s2,s0,s1\n\tstr q2,[%0]"::"r"(out),"r"(inputs):"v0","v1","v2","memory");}
 if(out[0]!=expected||out[1]){fprintf(stderr,"wide=%d alias=%d a=%llx b=%llx got=%llx:%llx expected=%llx\n",wide,alias,(unsigned long long)a,(unsigned long long)b,(unsigned long long)out[1],(unsigned long long)out[0],(unsigned long long)expected);assert(0);}
}
int main(void){
 const uint64_t d[]={0,0x8000000000000000ULL,0x3ff0000000000000ULL,0xbff0000000000000ULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000000ULL,1};
 const uint32_t s[]={0,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00000,1};
 unsigned n=0;for(unsigned i=0;i<8;i++)for(unsigned j=0;j<8;j++)for(int alias=0;alias<2;alias++){
 double x,y;memcpy(&x,&d[i],8);memcpy(&y,&d[j],8);check(d[i],d[j],1,alias,x==y?~0ULL:0);
 float a,b;memcpy(&a,&s[i],4);memcpy(&b,&s[j],4);check(s[i],s[j],0,alias,a==b?0xffffffffULL:0);n+=2;}
 printf("SCALAR_FCMEQ_PASS cases=%u full-register/aliases/zero/NaN/inf/subnormal\n",n);
}
