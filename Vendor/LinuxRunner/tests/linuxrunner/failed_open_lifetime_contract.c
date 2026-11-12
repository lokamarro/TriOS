#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>
static void fill_and_release(void) {
    int fds[64],n=0,fd;
    while((fd=open("/dev/null",O_RDONLY))>=0){assert(n<64);fds[n++]=fd;}
    assert(errno==EMFILE);
    for(int i=0;i<n;i++)assert(!close(fds[i]));
}
int main(void) {
    alarm(20);struct rlimit saved,small;
    assert(!getrlimit(RLIMIT_NOFILE,&saved));small=saved;small.rlim_cur=32;
    assert(!setrlimit(RLIMIT_NOFILE,&small));fill_and_release();
    for(int i=0;i<1000;i++) {
        char path[100];snprintf(path,sizeof path,"/etc/.lr-missing-%d-%d",getpid(),i);
        int fd=open(path,O_RDONLY);assert(fd==-1 && errno==ENOENT);
    }
    fill_and_release();assert(!setrlimit(RLIMIT_NOFILE,&saved));
    puts("FAILED_OPEN_LIFETIME_PASS missing=1000 bounded_guest_limit=32");
}
