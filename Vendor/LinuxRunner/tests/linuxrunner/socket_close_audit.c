// Test-only Darwin close observer: no guest behavior or production changes.
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
static int observed_close(int fd) {
    int result=close(fd),saved=errno;
    if(result<0 && saved==EBADF && fd>=0){char line[96];int n=snprintf(line,sizeof line,"HOST_CLOSE_EBADF fd=%d\n",fd);write(2,line,n);}
    errno=saved;return result;
}
__attribute__((used)) static struct {const void *replacement,*original;} interpose
__attribute__((section("__DATA,__interpose")))={(const void *)observed_close,(const void *)close};
