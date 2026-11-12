#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc,char **argv) {
    assert(argc==2);alarm(10);
    puts("SOCKET_FD_PHASE_BEGIN");fflush(stdout);
    if(!strcmp(argv[1],"efault")) {
        for(int i=0;i<20;i++) {errno=0;assert(syscall(SYS_socketpair,AF_UNIX,SOCK_STREAM,0,(void*)1)==-1 && errno==EFAULT);}
    } else {
        struct rlimit lim;assert(!getrlimit(RLIMIT_NOFILE,&lim));lim.rlim_cur=32;assert(!setrlimit(RLIMIT_NOFILE,&lim));
        int fds[64],n=0,fd;while((fd=open("/dev/null",O_RDONLY))>=0){assert(n<64);fds[n++]=fd;}assert(errno==EMFILE);
        if(!strcmp(argv[1],"one-slot"))assert(!close(fds[--n]));
        for(int i=0;i<20;i++) {
            errno=0;
            if(!strcmp(argv[1],"socket"))assert(socket(AF_INET,SOCK_STREAM,0)==-1 && errno==EMFILE);
            else {int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==-1 && errno==EMFILE);}
        }
        for(int i=0;i<n;i++){assert(fcntl(fds[i],F_GETFD)>=0);assert(!close(fds[i]));}
    }
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));char byte='x',readback=0;
    assert(write(pair[0],&byte,1)==1 && read(pair[1],&readback,1)==1 && readback=='x');assert(!close(pair[0])&&!close(pair[1]));
    puts("SOCKET_FD_PHASE_END");puts("SOCKET_FD_FAILURE_PASS");fflush(stdout);
}
