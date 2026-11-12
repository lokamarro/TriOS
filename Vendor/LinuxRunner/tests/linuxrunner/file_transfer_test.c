#include "app/LinuxRunner/FileTransfer.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char folder[] = "/tmp/linuxrunner-transfer-XXXXXX";
    assert(mkdtemp(folder));
    int directory = open(folder, O_RDONLY | O_DIRECTORY);
    int source = openat(directory, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(write(source, "linuxrunner\n", 12) == 12);
    lseek(source, 0, SEEK_SET);
    assert(lr_copy_in(source, directory, "imported") == 0);
    assert(!lr_valid_leaf("../escape")); assert(!lr_valid_leaf("."));
    assert(!lr_valid_leaf("..")); assert(!lr_valid_leaf("a\\b"));
    assert(lr_copy_in(source, directory, "../escape") == EINVAL);
    lseek(source, 0, SEEK_SET);
    assert(lr_copy_in(source, directory, "imported") == EEXIST);
    int output = openat(directory, "export", O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(lr_copy_out(directory, "imported", output) == 0);
    char bytes[20] = {0}; lseek(output, 0, SEEK_SET);
    assert(read(output, bytes, sizeof(bytes)) == 12 && !strcmp(bytes, "linuxrunner\n"));
    assert(symlinkat("source", directory, "symlink") == 0);
    assert(lr_copy_out(directory, "symlink", output) == ELOOP);
    assert(mkfifoat(directory, "fifo", 0600) == 0);
    assert(lr_copy_out(directory, "fifo", output) == EINVAL);
    assert(lr_copy_out(directory, "missing", output) == ENOENT);
    assert(ftruncate(source, 513LL * 1024 * 1024) == 0);
    lseek(source, 0, SEEK_SET);
    assert(lr_copy_in(source, directory, "too-large") == EFBIG);
    assert(faccessat(directory, "too-large", F_OK, 0) == -1);
    close(source); close(output);
    for (const char **p = (const char *[]){"source","imported","export","symlink","fifo",NULL}; *p; p++) unlinkat(directory, *p, 0);
    close(directory); assert(rmdir(folder) == 0);
    puts("PASS: import/export content, traversal, collision, symlink, FIFO, missing, size limit, staging cleanup");
}
