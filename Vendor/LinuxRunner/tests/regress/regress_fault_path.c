#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
int main(void) {
    pid_t child=fork();
    if(child==0) {
        int fd=open("/dev/shm/linuxrunner-intentionally-absent-diagnostic",O_RDONLY);
        if(fd>=0 || errno!=ENOENT) _exit(1);
        __asm__ volatile("brk #7");
        _exit(2);
    }
    int status=0;
    if(child<0 || waitpid(child,&status,0)!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGTRAP) return 1;
    puts("FAULT_PATH_SIGNAL_PASS: real SIGTRAP delivered; parent survives");
    return 0;
}
