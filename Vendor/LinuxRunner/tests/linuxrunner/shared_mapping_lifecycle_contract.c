#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned census(const char *tag) {
    FILE *f = fopen("/proc/ish/fds", "r"); assert(f);
    char line[640]; unsigned live = ~0U;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "FD_TREND ", 9)) {
        char *p = strstr(line, " descriptions="); assert(p);
        live = strtoul(p + 14, NULL, 10); printf("%s %s", tag, line);
    }
    fclose(f); assert(live != ~0U); return live;
}
int main(void) {
    const size_t length = 262144, offset = 4096;
    unsigned before = census("BEFORE");
    int fd = syscall(SYS_memfd_create, "shared-lifecycle", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(fd >= 0 && !ftruncate(fd, length + offset));
    assert(!fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));
    for (unsigned n = 0; n < 1024; n++) {
        unsigned char *first = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        unsigned char *second = mmap((void *)(64ULL << 30), length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset);
        assert(first != MAP_FAILED && second != MAP_FAILED);
        first[offset + 35] = n;
        assert(second[35] == (unsigned char)n);
        unsigned char observed; assert(pread(fd, &observed, 1, offset + 35) == 1 && observed == (unsigned char)n);
        if (!(n % 256)) {
            pid_t child = fork(); assert(child >= 0);
            if (!child) { close(fd); second[36] = 91; _exit(0); }
            int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
            assert(first[offset + 36] == 91);
        }
        assert(!munmap(first, length) && !munmap(second, length));
    }
    unsigned char *last = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset);
    assert(last != MAP_FAILED); close(fd); last[35] = 77; assert(last[35] == 77); assert(!munmap(last, length));
    unsigned after = census("AFTER"); assert(after <= before + 2);
    puts("SHARED_MAPPING_LIFECYCLE_PASS cycles=1024 length=262144 offset=4096 sealed_alias_fork_close_and_cleanup");
    return 0;
}
