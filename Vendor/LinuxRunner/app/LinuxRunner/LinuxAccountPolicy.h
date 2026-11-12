#ifndef LR_LINUX_ACCOUNT_POLICY_H
#define LR_LINUX_ACCOUNT_POLICY_H
#include <stdbool.h>
#include <stddef.h>
// Deliberately bounded portable names, never shell syntax or option prefixes.
static inline bool lr_linux_account_name_valid(const char *name) {
    if (!name || !((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return false;
    size_t n = 0;
    for (; name[n]; ++n) {
        if (n >= 32) return false;
        char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return n != 0;
}
#endif
