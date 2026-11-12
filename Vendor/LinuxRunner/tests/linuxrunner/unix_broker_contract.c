#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"BROKER_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static int lifetime[2],control[2];
static void *broker(void *unused){(void)unused;struct pollfd p[2]={{.fd=lifetime[0],.events=POLLIN},{.fd=control[0],.events=POLLIN}};
 CHECK(poll(p,2,3000)==1);printf("BROKER_POLL life=%x control=%x\n",p[0].revents,p[1].revents);CHECK(!p[0].revents&&p[1].revents==POLLIN);
 char byte=0,buf[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&byte,1};struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=buf,.msg_controllen=sizeof buf};CHECK(recvmsg(control[0],&msg,0)==1&&byte=='T');struct cmsghdr *c=CMSG_FIRSTHDR(&msg);CHECK(c&&c->cmsg_type==SCM_RIGHTS);int reply;memcpy(&reply,CMSG_DATA(c),sizeof reply);CHECK(send(reply,"UTC",3,MSG_NOSIGNAL)==3);close(reply);return NULL;}
int main(void){setbuf(stdout,NULL);alarm(15);CHECK(pipe(lifetime)==0&&socketpair(AF_UNIX,SOCK_SEQPACKET,0,control)==0);CHECK(shutdown(control[1],SHUT_RD)==0&&shutdown(control[0],SHUT_WR)==0);pthread_t thread;CHECK(pthread_create(&thread,NULL,broker,NULL)==0);pid_t child=fork();CHECK(child>=0);if(!child){close(lifetime[0]);close(lifetime[1]);close(control[0]);int reply[2];CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET,0,reply)==0);char byte='T',buf[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&byte,1};struct msghdr msg={.msg_iov=&io,.msg_iovlen=1,.msg_control=buf,.msg_controllen=CMSG_LEN(sizeof(int))};struct cmsghdr*c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&reply[1],sizeof(int));CHECK(sendmsg(control[1],&msg,MSG_NOSIGNAL)==1);close(reply[1]);char text[4]={0};CHECK(recv(reply[0],text,3,0)==3&&!strcmp(text,"UTC"));close(reply[0]);close(control[1]);_exit(0);}close(control[1]);CHECK(pthread_join(thread,NULL)==0);int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));close(lifetime[1]);close(lifetime[0]);close(control[0]);puts("UNIX_BROKER_PASS lifeline fork seqpacket SCM_RIGHTS response endpoint lifetime");return 0;}
