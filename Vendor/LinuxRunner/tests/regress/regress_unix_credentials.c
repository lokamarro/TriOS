#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static void send_message(int fd,int with_fd){
 char b='X';struct iovec iov={&b,1};char control[CMSG_SPACE(sizeof(int))]={0};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1};
 int f=-1;if(with_fd){f=open("/etc/os-release",O_RDONLY);assert(f>=0);m.msg_control=control;m.msg_controllen=sizeof control;struct cmsghdr*c=CMSG_FIRSTHDR(&m);c->cmsg_len=CMSG_LEN(sizeof(int));c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;memcpy(CMSG_DATA(c),&f,sizeof f);}assert(sendmsg(fd,&m,0)==1);if(f>=0)close(f);
}
static void receive_message(int fd,pid_t pid,int with_fd,int flags){
 char b=0,control[256]={0};struct iovec iov={&b,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};assert(recvmsg(fd,&m,flags)==1&&b=='X');int creds=0,rights=0;
 for(struct cmsghdr*c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)){assert(c->cmsg_level==SOL_SOCKET);if(c->cmsg_type==SCM_CREDENTIALS){struct ucred u;assert(c->cmsg_len==CMSG_LEN(sizeof u));memcpy(&u,CMSG_DATA(c),sizeof u);assert(u.pid==pid&&u.uid==geteuid()&&u.gid==getegid());creds++;}else if(c->cmsg_type==SCM_RIGHTS){int f;memcpy(&f,CMSG_DATA(c),sizeof f);char q;assert(read(f,&q,1)==1);if(flags&MSG_CMSG_CLOEXEC)assert(fcntl(f,F_GETFD)&FD_CLOEXEC);close(f);rights++;}else assert(0);}
 assert(creds==1&&rights==with_fd&&!(m.msg_flags&MSG_CTRUNC));
}
static void concurrent_senders(void){
 int sv[2],on=1;assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));assert(!setsockopt(sv[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof on));
 pid_t kids[2];for(int j=0;j<2;j++){kids[j]=fork();assert(kids[j]>=0);if(!kids[j]){for(int i=0;i<50;i++){char b='A'+j;struct iovec v={&b,1};struct msghdr m={.msg_iov=&v,.msg_iovlen=1};assert(sendmsg(sv[1],&m,0)==1);}_exit(0);}}
 int counts[2]={0};for(int i=0;i<100;i++){char b,control[128];struct iovec v={&b,1};struct msghdr m={.msg_iov=&v,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};assert(recvmsg(sv[0],&m,0)==1);assert(b=='A'||b=='B');struct cmsghdr*c=CMSG_FIRSTHDR(&m);assert(c&&c->cmsg_type==SCM_CREDENTIALS);struct ucred u;memcpy(&u,CMSG_DATA(c),sizeof u);assert(u.pid==kids[b-'A']);counts[b-'A']++;}
 for(int j=0;j<2;j++){int status;assert(counts[j]==50&&waitpid(kids[j],&status,0)==kids[j]&&WIFEXITED(status)&&!WEXITSTATUS(status));}close(sv[0]);close(sv[1]);
}
int main(void){alarm(20);setbuf(stdout,NULL);int sv[2],on=1;assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sv));assert(!setsockopt(sv[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof on));socklen_t n=sizeof on;on=0;assert(!getsockopt(sv[0],SOL_SOCKET,SO_PASSCRED,&on,&n)&&on==1);
 struct ucred peer;n=sizeof peer;assert(!getsockopt(sv[0],SOL_SOCKET,SO_PEERCRED,&peer,&n)&&peer.pid==getpid());
 send_message(sv[1],0);receive_message(sv[0],getpid(),0,MSG_PEEK);receive_message(sv[0],getpid(),0,0);
 send_message(sv[1],1);receive_message(sv[0],getpid(),1,0);
 send_message(sv[1],1);receive_message(sv[0],getpid(),1,MSG_CMSG_CLOEXEC);
 pid_t child=fork();assert(child>=0);if(!child){send_message(sv[1],0);_exit(0);}receive_message(sv[0],child,0,0);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 assert(write(sv[1],"X",1)==1);receive_message(sv[0],getpid(),0,0);
 assert(send(sv[1],"X",1,0)==1);receive_message(sv[0],getpid(),0,0);
 send_message(sv[1],1);char b;struct iovec iov={&b,1};struct msghdr m={.msg_iov=&iov,.msg_iovlen=1};assert(recvmsg(sv[0],&m,0)==1&&(m.msg_flags&MSG_CTRUNC));
 send_message(sv[1],0);assert(read(sv[0],&b,1)==1);send_message(sv[1],0);receive_message(sv[0],getpid(),0,0);
 on=0;assert(!setsockopt(sv[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof on));send_message(sv[1],0);char control[128];m=(struct msghdr){.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};assert(recvmsg(sv[0],&m,0)==1&&m.msg_controllen==0);close(sv[0]);close(sv[1]);concurrent_senders();puts("UNIX_CREDENTIALS_PASS concurrent senders, CLOEXEC,  sender PID/UID/GID, child, peer, rights, peek, write/send, truncation/discard, disable");
}
