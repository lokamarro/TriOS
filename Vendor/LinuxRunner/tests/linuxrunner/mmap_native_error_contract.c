#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
    int f = memfd_create("native-error", MFD_CLOEXEC);
    assert(f >= 0 && !ftruncate(f, 262144));
    void *p = mmap(NULL, 262144, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    int error = errno;
    if (p != MAP_FAILED) munmap(p, 262144);
    close(f);
    printf("MMAP_NATIVE_ERROR observed=%d expected=%d\n", error, EACCES);
    return p == MAP_FAILED && error == EACCES ? 0 : 1;
}
