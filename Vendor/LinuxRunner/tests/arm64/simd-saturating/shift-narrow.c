// Scalar shift/narrow differential oracle. Original libwebp site is included.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } vec;
typedef void (*operation)(uint64_t, vec *, uint64_t *, uint64_t *, uint64_t);
#define VAR(OP,D,S,B,N,K,A,RD,RN,QD) \
static void OP##_##D##N##_##A(uint64_t input, vec *out, uint64_t *fpsr, \
        uint64_t *nzcv, uint64_t initial) { \
    vec source = {input, UINT64_C(0xabcdef0123456789)}; \
    __asm__ volatile("ldr q0, [%[source]]\n movi v1.16b, #255\n" \
        "msr fpsr, %[initial]\n msr nzcv, %[flags]\n" \
        #OP " " #D RD ", " #S RN ", #" #N "\n" \
        "str " QD ", [%[out]]\n mrs %[fpsr], fpsr\n mrs %[nzcv], nzcv\n" \
        : [fpsr] "=r" (*fpsr), [nzcv] "=r" (*nzcv) \
        : [source] "r" (&source), [out] "r" (out), [initial] "r" (initial), \
          [flags] "r" (UINT64_C(0xa0000000)) : "v0", "v1", "memory", "cc"); \
}
#define SET(OP,D,S,B,N,K) VAR(OP,D,S,B,N,K,distinct,"1","0","q1") VAR(OP,D,S,B,N,K,alias,"0","0","q0")
#define FAMILY(OP,K) \
    SET(OP,b,h,8,1,K) SET(OP,b,h,8,4,K) SET(OP,b,h,8,8,K) \
    SET(OP,h,s,16,1,K) SET(OP,h,s,16,4,K) SET(OP,h,s,16,16,K) \
    SET(OP,s,d,32,1,K) SET(OP,s,d,32,4,K) SET(OP,s,d,32,32,K)
FAMILY(uqrshrn,0) FAMILY(sqrshrn,1) FAMILY(uqshrn,2)
FAMILY(sqshrn,3) FAMILY(sqshrun,4) FAMILY(sqrshrun,5)

static void webp_site(uint64_t input, vec *out, uint64_t *fpsr,
        uint64_t *nzcv, uint64_t initial) {
    vec source = {input, UINT64_MAX};
    __asm__ volatile("ldr q31, [%[source]]\n msr fpsr, %[initial]\n msr nzcv, %[flags]\n"
        ".inst 0x7f0c9fff\n str q31, [%[out]]\n mrs %[fpsr], fpsr\n mrs %[nzcv], nzcv\n"
        : [fpsr] "=r" (*fpsr), [nzcv] "=r" (*nzcv)
        : [source] "r" (&source), [out] "r" (out), [initial] "r" (initial),
          [flags] "r" (UINT64_C(0xa0000000)) : "v31", "memory", "cc");
}
struct item { operation fn; unsigned kind, bits, shift; const char *name; };
#define ROW(OP,D,B,N,K) {OP##_##D##N##_distinct,K,B,N,#OP}, {OP##_##D##N##_alias,K,B,N,#OP}
#define TABLE(OP,K) ROW(OP,b,8,1,K),ROW(OP,b,8,4,K),ROW(OP,b,8,8,K), \
    ROW(OP,h,16,1,K),ROW(OP,h,16,4,K),ROW(OP,h,16,16,K), \
    ROW(OP,s,32,1,K),ROW(OP,s,32,4,K),ROW(OP,s,32,32,K)
static const struct item items[] = {TABLE(uqrshrn,0),TABLE(sqrshrn,1),TABLE(uqshrn,2),
    TABLE(sqshrn,3),TABLE(sqshrun,4),TABLE(sqrshrun,5),{webp_site,0,8,4,"webp-7f0c9fff"}};

int main(int argc, char **argv) {
    unsigned count=0;
    const uint64_t values[]={0,1,7,8,15,16,127,128,255,256,65535,65536,
        UINT64_C(0xffffffff),UINT64_C(0x100000000),UINT64_MAX,UINT64_MAX-1,
        UINT64_C(0x8000000000000000),UINT64_C(0x7fffffffffffffff)};
    unsigned begin=(argc==2&&!strcmp(argv[1],"--webp-site"))?sizeof(items)/sizeof(*items)-1:0;
    for (unsigned i=begin;i<sizeof(items)/sizeof(*items);i++) {
        const struct item *t=&items[i]; unsigned wide=t->bits*2;
        unsigned __int128 limit=(unsigned __int128)1<<wide;
        uint64_t mask=(uint64_t)(limit-1), destmask=(uint64_t)(((unsigned __int128)1<<t->bits)-1);
        uint64_t boundary=destmask<<t->shift;
        for(unsigned j=0;j<sizeof(values)/sizeof(*values)+3;j++) for(unsigned sticky=0;sticky<2;sticky++) {
            uint64_t raw=(j<sizeof(values)/sizeof(*values)?values[j]:boundary+j-sizeof(values)/sizeof(*values))&mask;
            int signed_input=t->kind==1||t->kind==3||t->kind==4||t->kind==5;
            int signed_output=t->kind==1||t->kind==3;
            __int128 value=raw;
            if(signed_input&&(raw&(UINT64_C(1)<<(wide-1))))value-=(__int128)limit;
            if(t->kind==0||t->kind==1||t->kind==5)value+=(__int128)1<<(t->shift-1);
            // AArch64 GCC/Clang implement arithmetic signed right shift.
            value>>=t->shift;
            __int128 low=signed_output?-((__int128)1<<(t->bits-1)):0;
            __int128 high=signed_output?((__int128)1<<(t->bits-1))-1:destmask;
            int saturated=value<low||value>high;
            if(value<low)value=low; if(value>high)value=high;
            uint64_t expected=(uint64_t)value&destmask;
            uint64_t initial=UINT64_C(0x10)|(sticky?UINT64_C(1)<<27:0),fpsr,nzcv;
            vec out; t->fn(raw,&out,&fpsr,&nzcv,initial);count++;
            uint64_t expected_flags=initial|(saturated?UINT64_C(1)<<27:0);
            if(out.lo!=expected||out.hi||fpsr!=expected_flags||nzcv!=UINT64_C(0xa0000000)) {
                fprintf(stderr,"FAIL case=%u %s bits=%u shift=%u input=%016" PRIx64
                    " result=%016" PRIx64 "/%016" PRIx64 " expected=%016" PRIx64
                    " fpsr=%08" PRIx64 "/%08" PRIx64 " nzcv=%08" PRIx64 "\n",
                    count,t->name,t->bits,t->shift,raw,out.hi,out.lo,expected,fpsr,expected_flags,nzcv);
                return 1;
            }
        }
    }
    printf("SCALAR_SHIFT_NARROW_PASS cases=%u webp_site=PASS aliases=PASS upper_clear=PASS FPSR_QC=PASS NZCV=PASS\n",count);
    return 0;
}
