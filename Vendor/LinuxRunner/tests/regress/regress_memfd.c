#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void){
 int fd=memfd_create("regression",MFD_CLOEXEC);assert(fd>=0);assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);
 struct stat st;assert(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size==0&&st.st_uid==geteuid()&&st.st_gid==getegid());
 assert(!ftruncate(fd,8192));assert(!fstat(fd,&st)&&st.st_size==8192);
 assert(pwrite(fd,"shared",7,4096)==7);char b[7];assert(pread(fd,b,7,4096)==7&&!strcmp(b,"shared"));
 int *p=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(p!=MAP_FAILED);*p=1;
 pid_t child=fork();assert(child>=0);if(!child){*p=42;_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&*p==42);
 assert(!munmap(p,8192));assert(!ftruncate(fd,3));assert(!fstat(fd,&st)&&st.st_size==3);
 errno=0;assert(ftruncate(fd,-1)==-1&&errno==EINVAL);
 close(fd);int fds[2];assert(!pipe(fds));errno=0;assert(ftruncate(fds[1],100)==-1&&errno==EINVAL);close(fds[0]);close(fds[1]);puts("memfd growth/shrink/stat/shared fork/pipe rejection PASS");
}
