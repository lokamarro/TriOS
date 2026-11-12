#define _GNU_SOURCE
// Metadata-cache coherence on the fakefs root: every metadata write must be
// visible to the next lookup (chmod, chown, rename of a subtree, link,
// unlink, symlink, mkdir/rmdir, replace-by-rename), including paths that were
// looked up (and cached) just before the change, and negative lookups.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf(" errno=%d\n", errno); } } while (0)
static mode_t mode_of(const char *p) { struct stat st; return lstat(p, &st) == 0 ? st.st_mode & 07777 : (mode_t) -1; }
static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char base[] = "/tmp/lr-cache-XXXXXX"; CHECK(mkdtemp(base) != NULL, "mkdtemp");
    char a[256], b[256], d[256], d2[256], child[256], moved[256], hard[256], sym[256], buf[256];
    snprintf(a, sizeof a, "%s/a", base); snprintf(b, sizeof b, "%s/b", base);
    snprintf(d, sizeof d, "%s/dir", base); snprintf(d2, sizeof d2, "%s/dir2", base);
    snprintf(child, sizeof child, "%s/dir/child", base); snprintf(moved, sizeof moved, "%s/dir2/child", base);
    snprintf(hard, sizeof hard, "%s/hard", base); snprintf(sym, sizeof sym, "%s/sym", base);
    for (int round = 0; round < 50; round++) {
        CHECK(!exists(a), "negative lookup before create (round %d)", round);
        int fd = open(a, O_CREAT | O_WRONLY | O_EXCL, 0640); CHECK(fd >= 0, "create"); close(fd);
        CHECK(mode_of(a) == 0640, "mode after create %o", mode_of(a));
        CHECK(chmod(a, 0604) == 0 && mode_of(a) == 0604, "chmod visible %o", mode_of(a));
        CHECK(mkdir(d, 0750) == 0 && mode_of(d) == 0750, "mkdir");
        fd = open(child, O_CREAT | O_WRONLY, 0611); CHECK(fd >= 0, "child"); close(fd);
        CHECK(mode_of(child) == 0611, "child mode %o", mode_of(child));
        CHECK(rename(d, d2) == 0, "rename dir");
        CHECK(!exists(child) && mode_of(moved) == 0611, "subtree rename: old %d new %o", exists(child), mode_of(moved));
        CHECK(link(a, hard) == 0 && mode_of(hard) == 0604, "hard link mode %o", mode_of(hard));
        CHECK(chmod(hard, 0600) == 0 && mode_of(a) == 0600, "chmod through link visible on other name %o", mode_of(a));
        CHECK(symlink("a", sym) == 0, "symlink");
        ssize_t n = readlink(sym, buf, sizeof buf - 1); if (n >= 0) buf[n] = 0;
        CHECK(n == 1 && buf[0] == 'a', "readlink");
        CHECK(stat(sym, &(struct stat){0}) == 0 && mode_of(a) == 0600, "follow symlink");
        fd = open(b, O_CREAT | O_WRONLY, 0444); CHECK(fd >= 0, "b"); close(fd);
        CHECK(rename(b, a) == 0 && mode_of(a) == 0444, "replace by rename: mode %o", mode_of(a));
        CHECK(mode_of(hard) == 0600, "old inode keeps its mode via hard link %o", mode_of(hard));
        CHECK(unlink(sym) == 0 && !exists(sym), "unlink symlink");
        CHECK(unlink(a) == 0 && !exists(a), "unlink");
        CHECK(unlink(hard) == 0 && unlink(moved) == 0 && rmdir(d2) == 0 && !exists(d2), "cleanup");
        if (failures) break;
    }
    rmdir(base);
    printf("fakefs_cache_coherence_contract %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
