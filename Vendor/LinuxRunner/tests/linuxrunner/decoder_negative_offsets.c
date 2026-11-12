#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    uint64_t pc,page,literal,loaded;
    __asm__ volatile("adr %[pc], 1f\n 1: .inst 0xf0ffffe5\n mov %[page], x5\n"
        : [pc] "=&r"(pc),[page] "=r"(page) :: "x5");
    assert(page==(pc&~UINT64_C(4095))-4096);
    __asm__ volatile("b 2f\n .p2align 3\n 1: .quad 0x1020304050607080\n"
        "2: ldr %[value], 1b\n" : [value] "=r"(literal));
    assert(literal==UINT64_C(0x1020304050607080));
    uint64_t words[3]={literal,0,0},*base=words+1;
    __asm__ volatile("ldur %[out], [%[base], #-8]\n"
        : [out] "=r"(loaded) : [base] "r"(base) : "memory");
    assert(loaded==literal);
    uint64_t value=UINT64_C(0x8877665544332211);
    __asm__ volatile("stur %[value], [%[base], #-8]\n"
        :: [value] "r"(value),[base] "r"(base) : "memory");
    assert(words[0]==value&&words[1]==0&&words[2]==0);
    puts("DECODER_NEGATIVE_OFFSETS_PASS ADRP=PASS literal=PASS LDUR=PASS STUR=PASS");
    return 0;
}
