#define _GNU_SOURCE
// File mappings keep file identity but no open descriptor, as on Linux.
// Build 882 phone VS Code: every mapped file pinned one host fd per process,
// exhausting the iOS soft limit (host_open=2559/2560) until Chromium's shared
// memory allocation failed (OnNoMemoryInternal). Checks host descriptor growth
// via /proc/ish/fds (runtime census; needs root), /proc/self/maps names, data,
// and a shared futex on a file-backed MAP_SHARED mapping across fork.
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)

static long host_open(void) {
    FILE *f = fopen("/proc/ish/fds", "r");
    if (!f) return -1;
    char line[512]; long value = -1;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "FD_TREND"); if (!p) continue;
        p = strstr(line, "host_open="); if (p) value = strtol(p + 10, NULL, 10);
    }
    fclose(f);
    return value;
}
static int maps_mentions(const char *path) {
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return 0;
    char line[1024]; int found = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, path)) { found = 1; break; }
    fclose(f);
    return found;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[] = "/tmp/lr-mapping-identity-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0, "mkstemp");
    char page[4096]; memset(page, 'L', sizeof page);
    CHECK(write(fd, page, sizeof page) == (ssize_t) sizeof page && ftruncate(fd, 8192) == 0, "prepare file");
    close(fd);

    enum { N = 200 };
    static void *maps[N];
    long before = host_open();
    for (int i = 0; i < N; i++) {
        int f = open(path, O_RDONLY); CHECK(f >= 0, "open %d", i);
        maps[i] = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, f, 0);
        CHECK(maps[i] != MAP_FAILED, "mmap %d", i);
        close(f);
    }
    long after = host_open();
    if (before >= 0 && after >= 0)
        CHECK(after - before < 20, "host descriptors grew %ld for %d closed file mappings", after - before, N);
    else printf("note: /proc/ish/fds unavailable (non-root); host growth not measured\n");
    CHECK(((char *) maps[N - 1])[100] == 'L', "mapped data");
    CHECK(maps_mentions(path), "/proc/self/maps lost the file name");
    char exe[1024] = {0};
    CHECK(readlink("/proc/self/exe", exe, sizeof exe - 1) > 0 && maps_mentions(exe), "executable mapping name '%s'", exe);
    for (int i = 0; i < N; i++) munmap(maps[i], 4096);

    // Shared futex through a file-backed MAP_SHARED mapping keyed by file identity.
    int f = open(path, O_RDWR); CHECK(f >= 0, "open rw");
    uint32_t *word = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, 4096);
    CHECK(word != MAP_FAILED, "shared map");
    close(f);
    *word = 0;
    pid_t child = fork();
    if (child == 0) {
        while (__atomic_load_n(word, __ATOMIC_SEQ_CST) == 0)
            syscall(SYS_futex, word, FUTEX_WAIT, 0, NULL, NULL, 0);
        _exit(*word == 7 ? 0 : 1);
    }
    usleep(200000);
    __atomic_store_n(word, 7, __ATOMIC_SEQ_CST);
    int woken = 0;
    for (int i = 0; i < 50 && !woken; i++) { woken = (int) syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0); if (!woken) usleep(20000); }
    int status = 0; waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "shared futex child status=%#x", status);
    munmap(word, 4096);
    unlink(path);
    printf("mapping_fd_retention_contract %s (host_open %ld -> %ld for %d mappings)\n", failures ? "FAIL" : "PASS", before, after, N);
    return failures ? 1 : 0;
}
