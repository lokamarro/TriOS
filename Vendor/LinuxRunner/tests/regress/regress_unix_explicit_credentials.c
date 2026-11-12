#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static ssize_t send_cred(int fd, struct ucred u, int rights, int malformed) {
    char b='C', control[CMSG_SPACE(sizeof u)+CMSG_SPACE(sizeof(int))]={0};
    struct iovec v={&b,1};
    struct msghdr m={.msg_iov=&v,.msg_iovlen=1,.msg_control=control,
        .msg_controllen=CMSG_SPACE(sizeof u)+(rights>=0?CMSG_SPACE(sizeof(int)):0)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&m);
    c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_CREDENTIALS;
    c->cmsg_len=CMSG_LEN(sizeof u)-(malformed?1:0);memcpy(CMSG_DATA(c),&u,sizeof u);
    if(rights>=0){c=CMSG_NXTHDR(&m,c);assert(c);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&rights,sizeof rights);}
    return sendmsg(fd,&m,MSG_NOSIGNAL);
}
static void receive_cred(int fd,pid_t pid,int rights,int flags) {
    char b=0,control[128]={0};struct iovec v={&b,1};
    struct msghdr m={.msg_iov=&v,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};
    assert(recvmsg(fd,&m,flags)==1&&b=='C'&&!(m.msg_flags&MSG_CTRUNC));int creds=0,fds=0;
    for(struct cmsghdr*c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)){
        if(c->cmsg_type==SCM_CREDENTIALS){struct ucred u;memcpy(&u,CMSG_DATA(c),sizeof u);assert(u.pid==pid&&u.uid==getuid()&&u.gid==getgid());creds++;}
        else if(c->cmsg_type==SCM_RIGHTS){int f;memcpy(&f,CMSG_DATA(c),sizeof f);assert(fcntl(f,F_GETFD)&FD_CLOEXEC);char q;assert(read(f,&q,1)==0);close(f);fds++;}
        else assert(0);
    }
    assert(creds==1&&fds==rights);
}
int main(void){
    alarm(20);int sv[2],on=1;assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));assert(!setsockopt(sv[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof on));
    struct ucred u={getpid(),getuid(),getgid()};
    assert(send_cred(sv[1],u,-1,0)==1);receive_cred(sv[0],getpid(),0,MSG_PEEK);receive_cred(sv[0],getpid(),0,0);
    int f=open("/dev/null",O_RDONLY);assert(f>=0);assert(send_cred(sv[1],u,f,0)==1);receive_cred(sv[0],getpid(),1,MSG_CMSG_CLOEXEC);close(f);
    errno=0;assert(send_cred(sv[1],u,-1,1)==-1&&errno==EINVAL);
    // Unprivileged callers cannot forge another identity.
    pid_t kid=fork();assert(kid>=0);if(!kid){assert(!setgid(123)&&!setuid(123));struct ucred forged={getpid(),124,123};errno=0;assert(send_cred(sv[1],forged,-1,0)==-1&&errno==EPERM);forged=(struct ucred){getpid(),123,123};assert(send_cred(sv[1],forged,-1,0)==1);_exit(0);}
    char b,ctl[128];struct iovec v={&b,1};struct msghdr m={.msg_iov=&v,.msg_iovlen=1,.msg_control=ctl,.msg_controllen=sizeof ctl};assert(recvmsg(sv[0],&m,0)==1);struct cmsghdr*c=CMSG_FIRSTHDR(&m);assert(c&&c->cmsg_type==SCM_CREDENTIALS);memcpy(&u,CMSG_DATA(c),sizeof u);assert(u.pid==kid&&u.uid==123&&u.gid==123);int status;assert(waitpid(kid,&status,0)==kid&&WIFEXITED(status)&&!WEXITSTATUS(status));
    on=0;assert(!setsockopt(sv[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof on));u=(struct ucred){getpid(),getuid(),getgid()};assert(send_cred(sv[1],u,-1,0)==1);m.msg_controllen=sizeof ctl;assert(recvmsg(sv[0],&m,0)==1&&!m.msg_controllen);
    close(sv[0]);close(sv[1]);puts("UNIX_EXPLICIT_CREDENTIALS_PASS own identity, rights, peek, child, invalid credentials, malformed, disabled receiver");
}
