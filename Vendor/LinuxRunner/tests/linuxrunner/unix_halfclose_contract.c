#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"HALFCLOSE_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static short ready(int fd,short mask,int ms){struct pollfd p={.fd=fd,.events=mask};CHECK(poll(&p,1,ms)>=0);return p.revents;}
static void run(int kind){
 int s[2];char c;CHECK(socketpair(AF_UNIX,kind,0,s)==0);
 CHECK(ready(s[0],POLLIN|POLLRDHUP,0)==0);
 int ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);struct epoll_event event={.events=EPOLLIN|EPOLLRDHUP,.data.u64=9};CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,s[0],&event)==0);
 CHECK(shutdown(s[0],SHUT_WR)==0);
 CHECK(ready(s[0],POLLIN|POLLRDHUP,20)==0);
 CHECK(epoll_wait(ep,&event,1,20)==0);
 short r=ready(s[1],POLLIN|POLLRDHUP,0);CHECK((r&(POLLIN|POLLRDHUP))==(POLLIN|POLLRDHUP)&&!(r&(POLLHUP|POLLPRI)));
 CHECK(recv(s[1],&c,1,0)==0);CHECK(send(s[1],"R",1,MSG_NOSIGNAL)==1);
 CHECK(epoll_wait(ep,&event,1,500)==1&&(event.events&EPOLLIN)&&!(event.events&EPOLLHUP));
 CHECK(recv(s[0],&c,1,0)==1&&c=='R');
 CHECK(shutdown(s[1],SHUT_WR)==0);
 r=ready(s[0],POLLIN|POLLRDHUP,0);CHECK((r&(POLLHUP|POLLIN|POLLRDHUP))==(POLLHUP|POLLIN|POLLRDHUP));
 close(ep);close(s[0]);close(s[1]);
 CHECK(socketpair(AF_UNIX,kind,0,s)==0);CHECK(shutdown(s[0],SHUT_RD)==0);
 r=ready(s[0],POLLIN|POLLRDHUP,0);CHECK((r&(POLLIN|POLLRDHUP))==(POLLIN|POLLRDHUP)&&!(r&POLLHUP));
 CHECK(ready(s[1],POLLIN|POLLRDHUP,20)==0);close(s[0]);
 r=ready(s[1],POLLIN|POLLRDHUP,0);CHECK((r&(POLLIN|POLLHUP))==(POLLIN|POLLHUP));close(s[1]);
 CHECK(socketpair(AF_UNIX,kind,0,s)==0);int dupfd=dup(s[0]);CHECK(dupfd>=0);CHECK(shutdown(dupfd,SHUT_WR)==0);close(dupfd);
 CHECK(ready(s[0],POLLIN,20)==0);CHECK(send(s[1],"D",1,MSG_NOSIGNAL)==1);close(s[1]);
 r=ready(s[0],POLLIN|POLLRDHUP,0);CHECK((r&(POLLIN|POLLHUP))==(POLLIN|POLLHUP));CHECK(recv(s[0],&c,1,0)==1&&c=='D');CHECK(recv(s[0],&c,1,0)==0);
 errno=0;CHECK(shutdown(s[0],3)==-1&&errno==EINVAL);close(s[0]);
 printf("HALFCLOSE_KIND_PASS kind=%d\n",kind);
}
int main(void){alarm(15);run(SOCK_STREAM);run(SOCK_SEQPACKET);int file=open("/dev/null",O_RDONLY);CHECK(file>=0);errno=0;CHECK(shutdown(file,SHUT_WR)==-1&&errno==ENOTSOCK);close(file);errno=0;CHECK(shutdown(-1,SHUT_WR)==-1&&errno==EBADF);printf("UNIX_HALFCLOSE_PASS checks=%u poll epoll fork_broker_separate dup EOF data shutdown_errors\n",checks);return 0;}
