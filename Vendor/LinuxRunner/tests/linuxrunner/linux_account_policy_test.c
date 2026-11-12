#include "app/LinuxRunner/LinuxAccountPolicy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *good[] = {"root", "reyhan", "_user", "user-42", "a1234567890123456789012345678901"};
    const char *bad[] = {NULL, "", "-root", "123", "Root", "user name", "u;id", "u\nroot", "u$(id)", "u'", "u/../root", "u:0", "a12345678901234567890123456789012", "\xc3\xa9"};
    for (unsigned i=0;i<sizeof good/sizeof *good;i++) assert(lr_linux_account_name_valid(good[i]));
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) assert(!lr_linux_account_name_valid(bad[i]));
    puts("LINUX_ACCOUNT_NAME_PASS");
}
