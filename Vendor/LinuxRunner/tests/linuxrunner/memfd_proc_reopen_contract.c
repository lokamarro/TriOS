#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int reopen_fd(int fd,int flags) {
    char path[80];snprintf(path,sizeof path,"/proc/self/fd/%d",fd);
    int next=open(path,flags);if(next<0)perror("MEMFD_PROC_REOPEN");assert(next>=0);return next;
}
static void transfer_readonly(int ro) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    pid_t child=fork();assert(child>=0);
    if(!child){
        close(sockets[0]);close(ro);char byte=0,control[CMSG_SPACE(sizeof(int))]={0};
        struct iovec io={&byte,1};struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
        assert(recvmsg(sockets[1],&m,MSG_CMSG_CLOEXEC)==1&&byte=='R');
        struct cmsghdr*c=CMSG_FIRSTHDR(&m);assert(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len==CMSG_LEN(sizeof(int)));
        int received;memcpy(&received,CMSG_DATA(c),sizeof received);
        assert(fcntl(received,F_GETFD)&FD_CLOEXEC);
        assert((fcntl(received,F_GETFL)&O_ACCMODE)==O_RDONLY);
        assert(read(received,&byte,1)==1&&byte==3);
        errno=0;assert(write(received,"x",1)==-1&&errno==EBADF);
        assert(!fcntl(received,F_SETFL,O_APPEND));
        assert(fcntl(received,F_GET_SEALS)>=0);close(received);close(sockets[1]);_exit(0);
    }
    close(sockets[1]);char byte='R',control[CMSG_SPACE(sizeof(int))]={0};
    struct iovec io={&byte,1};struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
    struct cmsghdr*c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&ro,sizeof ro);
    assert(sendmsg(sockets[0],&m,0)==1);close(sockets[0]);int status;
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(lseek(ro,0,SEEK_CUR)==4&&(fcntl(ro,F_GETFL)&O_APPEND));
}
static void exercise(void) {
    int fd=memfd_create("reopen",MFD_ALLOW_SEALING|MFD_CLOEXEC);assert(fd>=0);
    assert(!ftruncate(fd,16384));unsigned char data[64];for(unsigned i=0;i<sizeof data;i++)data[i]=i;
    assert(write(fd,data,sizeof data)==sizeof data);
    int ro=reopen_fd(fd,O_RDONLY|O_CLOEXEC),rw=reopen_fd(ro,O_RDWR);
    assert((fcntl(ro,F_GETFL)&O_ACCMODE)==O_RDONLY && (fcntl(ro,F_GETFD)&FD_CLOEXEC));
    assert(lseek(fd,0,SEEK_CUR)==64 && lseek(ro,0,SEEK_CUR)==0 && lseek(rw,0,SEEK_CUR)==0);
    unsigned char byte;assert(read(ro,&byte,1)==1 && byte==0);
    int duplicate=dup(ro);assert(duplicate>=0 && read(duplicate,&byte,1)==1 && byte==1);
    assert(lseek(ro,0,SEEK_CUR)==2 && lseek(fd,0,SEEK_CUR)==64);
    assert(!fcntl(ro,F_SETFL,O_NONBLOCK));assert(fcntl(duplicate,F_GETFL)&O_NONBLOCK);
    assert(!(fcntl(fd,F_GETFL)&O_NONBLOCK) && !(fcntl(rw,F_GETFL)&O_NONBLOCK));
    errno=0;assert(write(ro,"x",1)==-1&&errno==EBADF);
    errno=0;assert(pwrite(ro,"x",1,0)==-1&&errno==EBADF);
    errno=0;assert(ftruncate(ro,8192)==-1&&errno==EINVAL);
    errno=0;assert(fallocate(ro,0,0,4096)==-1&&errno==EBADF);
    int wo=reopen_fd(ro,O_WRONLY);errno=0;assert(read(wo,&byte,1)==-1&&errno==EBADF);
    errno=0;assert(pread(wo,&byte,1,0)==-1&&errno==EBADF);close(wo);
    assert(lseek(rw,17,SEEK_SET)==17 && write(rw,"z",1)==1);
    assert(pread(ro,&byte,1,17)==1&&byte=='z' && lseek(ro,0,SEEK_CUR)==2);
    int available=-1;assert(!ioctl(ro,FIONREAD,&available)&&available==16382);
    assert(!fchmod(rw,0640));struct stat st;assert(!fstat(ro,&st)&&(st.st_mode&0777)==0640);
    assert(!fchmod(rw,0600));
    struct flock held={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0,.l_len=1};
    assert(!fcntl(fd,F_OFD_SETLK,&held));
    errno=0;assert(fcntl(rw,F_OFD_SETLK,&held)==-1&&errno==EAGAIN);
    struct flock query=held;assert(!fcntl(ro,F_OFD_GETLK,&query)&&query.l_type==F_WRLCK&&query.l_pid==-1);
    held.l_type=F_UNLCK;assert(!fcntl(fd,F_OFD_SETLK,&held));
    pid_t child=fork();assert(child>=0);if(!child){assert(read(ro,&byte,1)==1&&byte==2);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(lseek(ro,0,SEEK_CUR)==3);
    transfer_readonly(ro);
    unsigned char *shared=mmap(NULL,16384,PROT_READ,MAP_SHARED,ro,0);assert(shared!=MAP_FAILED&&shared[17]=='z');
    errno=0;assert(mprotect(shared,16384,PROT_READ|PROT_WRITE)==-1&&errno==EACCES);
    errno=0;assert(mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_SHARED,ro,0)==MAP_FAILED&&errno==EACCES);
    pid_t mapping_child=fork();assert(mapping_child>=0);
    if(!mapping_child){errno=0;assert(mprotect(shared,16384,PROT_READ|PROT_WRITE)==-1&&errno==EACCES);_exit(0);}
    assert(waitpid(mapping_child,&status,0)==mapping_child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(mremap(shared,16384,8192,0)==shared);
    errno=0;assert(mprotect(shared,8192,PROT_READ|PROT_WRITE)==-1&&errno==EACCES);
    unsigned char *private=mmap(NULL,16384,PROT_READ,MAP_PRIVATE,ro,0);assert(private!=MAP_FAILED);
    assert(!mprotect(private,16384,PROT_READ|PROT_WRITE));private[17]='p';assert(shared[17]=='z');
    assert(!munmap(private,16384));
    assert(!fcntl(fd,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK));
    assert((fcntl(ro,F_GET_SEALS)&(F_SEAL_GROW|F_SEAL_SHRINK))==(F_SEAL_GROW|F_SEAL_SHRINK));
    errno=0;assert(ftruncate(rw,32768)==-1&&errno==EPERM);
    errno=0;assert(fcntl(ro,F_ADD_SEALS,F_SEAL_SEAL)==-1&&errno==EPERM);
    close(fd);close(rw);close(duplicate);
    rw=reopen_fd(ro,O_RDWR);assert(lseek(rw,0,SEEK_CUR)==0);
    assert(!fcntl(rw,F_ADD_SEALS,F_SEAL_SEAL));assert(fcntl(ro,F_GET_SEALS)&F_SEAL_SEAL);
    assert(pwrite(rw,"w",1,17)==1 && shared[17]=='w');close(rw);close(ro);
    assert(shared[17]=='w');assert(!munmap(shared,8192));
    fd=memfd_create("truncate-open",MFD_ALLOW_SEALING);assert(fd>=0);
    assert(write(fd,"data",4)==4);char path[80];snprintf(path,sizeof path,"/proc/self/fd/%d",fd);
    assert(!fchmod(fd,0400));
    if(geteuid()!=0){errno=0;assert(open(path,O_RDONLY|O_TRUNC)==-1&&errno==EACCES);assert(!fstat(fd,&st)&&st.st_size==4);}
    assert(!fchmod(fd,0600));ro=reopen_fd(fd,O_RDONLY|O_TRUNC);
    assert(!fstat(ro,&st)&&st.st_size==0&&(fcntl(ro,F_GETFL)&O_ACCMODE)==O_RDONLY);
    close(ro);assert(write(fd,"x",1)==1);assert(!fcntl(fd,F_ADD_SEALS,F_SEAL_SHRINK));
    errno=0;assert(open(path,O_RDONLY|O_TRUNC)==-1&&errno==EPERM);close(fd);
}
static unsigned census(const char*tag) {
    FILE*f=fopen("/proc/ish/fds","r");assert(f);char line[640];unsigned descriptions=~0U;
    while(fgets(line,sizeof line,f))if(!strncmp(line,"FD_TREND ",9)){char*p=strstr(line," descriptions=");assert(p);descriptions=strtoul(p+14,NULL,10);printf("%s %s",tag,line);}
    fclose(f);assert(descriptions!=~0U);return descriptions;
}
int main(void) {
    unsigned before=census("BEFORE");exercise();
    for(unsigned n=0;n<512;n++){
        int fd=memfd_create("reopen-loop",MFD_ALLOW_SEALING);assert(fd>=0);
        int ro=reopen_fd(fd,O_RDONLY),rw=reopen_fd(ro,O_RDWR);close(fd);
        assert(!ftruncate(rw,4096));assert(pwrite(rw,"x",1,0)==1);char byte;assert(pread(ro,&byte,1,0)==1&&byte=='x');
        close(rw);close(ro);
    }
    if(geteuid()==0){pid_t child=fork();assert(child>=0);if(!child){assert(!setgid(1001)&&!setuid(1001));exercise();_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
    unsigned after=census("AFTER");assert(after<=before+2);
    puts("MEMFD_PROC_REOPEN_PASS independent_offsets_status_modes_seals_metadata_fork_readonly_maps_CoW_lifetime_nonroot cycles=512");
}
