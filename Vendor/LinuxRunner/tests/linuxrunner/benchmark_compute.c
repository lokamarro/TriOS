#include <stdint.h>
#include <stdio.h>
int main(void) {
    uint32_t x = 0x12345678, sum = 0;
    for (unsigned i = 0; i < 1000000; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        sum += (x ^ i) * 33U;
    }
    printf("compute=%08x:%08x\n", x, sum);
    return 0;
}
