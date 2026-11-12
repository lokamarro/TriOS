#include <stdint.h>
#include <stdio.h>

struct decode_case {
    const char *name;
    uint32_t mask;
    uint32_t value;
    uint32_t match;
    uint32_t reject;
};

int main(void) {
    static const struct decode_case cases[] = {
        {"FCVTN/FCVTN2",  0xbfbffc00, 0x0e216800, 0x4e616800, 0x0ee16800},
        {"FCVTL/FCVTL2",  0xbfbffc00, 0x0e217800, 0x4e617800, 0x0ee17800},
        {"FCVTXN/FCVTXN2", 0xbffffc00, 0x2e616800, 0x6e616800, 0x2ee16800},
    };

    int failures = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct decode_case *test = &cases[i];
        if ((test->match & test->mask) != test->value) {
            fprintf(stderr, "%s: valid encoding does not match decoder\n", test->name);
            failures++;
        }
        if ((test->reject & test->mask) == test->value) {
            fprintf(stderr, "%s: decoder claims reserved/other encoding %#010x\n",
                    test->name, test->reject);
            failures++;
        }
    }
    if (failures != 0)
        return 1;
    puts("fcvt-decoder-mask-ok");
    return 0;
}
