// macOS test-only interposer: never linked or packaged into LinuxRunner.
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
static void *deny_shared(void *addr, size_t size, int prot, int flags, int fd, off_t off) {
    struct stat st;
    if (fd >= 0 && size == 262144 && (flags & MAP_SHARED) && !fstat(fd, &st) && st.st_nlink == 0 && st.st_size == 262144) {
        errno = EACCES; return MAP_FAILED;
    }
    return mmap(addr, size, prot, flags, fd, off);
}
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replace, *original; } pair = { (void *)deny_shared, (void *)mmap };
