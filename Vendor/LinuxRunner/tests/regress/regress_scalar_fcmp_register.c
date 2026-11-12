// Compare native ARM64 and interpreted scalar masks, aliases, inactive lanes
// and NZCV. Includes the exact Blender FCMGT S31,S30,S31 register encoding.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define COMPARE(type,rd,op) asm volatile("ldp q30,q31,[%2]\n\tmovi v29.16b,#0xff\n\tmsr nzcv,%3\n\t" op " " type rd "," type "30," type "31\n\tmrs %0,nzcv\n\tstr q" rd ",[%1]" : "=&r"(flags) : "r"(out),"r"(inputs),"r"(initial) : "v29","v30","v31","cc","memory")
#define CHOOSE(type,op) do {if(alias==0){COMPARE(type,"29",op);}else if(alias==1){COMPARE(type,"30",op);}else {COMPARE(type,"31",op);}}while(0)
static void check(uint64_t a,uint64_t b,int wide,int alias,int op,uint64_t expected){
 uint64_t inputs[4]={a,~0ULL,b,~0ULL},out[2],flags,initial=0xa0000000;
 if(wide){if(op==0){CHOOSE("d","fcmeq");}else if(op==1){CHOOSE("d","fcmge");}else {CHOOSE("d","fcmgt");}}
 else {if(op==0){CHOOSE("s","fcmeq");}else if(op==1){CHOOSE("s","fcmge");}else {CHOOSE("s","fcmgt");}}
 if(out[0]!=expected||out[1]||flags!=initial){fprintf(stderr,"CMP_FAIL wide=%d alias=%d op=%d got=%llx:%llx expected=%llx flags=%llx\n",wide,alias,op,(unsigned long long)out[1],(unsigned long long)out[0],(unsigned long long)expected,(unsigned long long)flags);assert(0);}
}
int main(void){
 const uint64_t d[]={0,0x8000000000000000ULL,0x3ff0000000000000ULL,0xbff0000000000000ULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000000ULL,1};
 const uint32_t s[]={0,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00000,1};
 unsigned n=0;for(unsigned i=0;i<8;i++)for(unsigned j=0;j<8;j++)for(int alias=0;alias<3;alias++)for(int op=0;op<3;op++){
 double x,y;memcpy(&x,&d[i],8);memcpy(&y,&d[j],8);check(d[i],d[j],1,alias,op,(op==0?x==y:op==1?x>=y:x>y)?~0ULL:0);
 float a,b;memcpy(&a,&s[i],4);memcpy(&b,&s[j],4);check(s[i],s[j],0,alias,op,(op==0?a==b:op==1?a>=b:a>b)?0xffffffffULL:0);n+=2;}
 printf("SCALAR_FCMP_REGISTER_PASS cases=%u EQ/GE/GT full-register/aliases/NZCV/zero/NaN/inf/subnormal\n",n);
}
