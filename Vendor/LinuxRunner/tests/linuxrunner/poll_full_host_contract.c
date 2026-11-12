#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/select.h>
#include <unistd.h>
int main(void){int fds[4096],n=0,p[2];assert(!pipe(p));assert(write(p[1],"x",1)==1);while(n<4096){int f=open("/tmp/lr-poll-full-host",O_CREAT|O_RDWR,0600);if(f<0)break;fds[n++]=f;}assert(n<4096&&errno==EMFILE);struct pollfd read={.fd=p[0],.events=POLLIN};errno=0;int a=poll(&read,1,0),ae=errno;fd_set set;FD_ZERO(&set);FD_SET(p[0],&set);struct timeval t={0};errno=0;int b=select(p[0]+1,&set,0,0,&t),be=errno;errno=0;int c=poll(0,0,2),ce=errno;struct pollfd duplicate[128];for(int i=0;i<128;i++)duplicate[i]=(struct pollfd){.fd=p[1],.events=POLLIN};errno=0;int d=poll(duplicate,128,2),de=errno;for(int i=0;i<n;i++)close(fds[i]);close(p[0]);close(p[1]);unlink("/tmp/lr-poll-full-host");printf("POLL_FULL_HOST opened=%d ready=%d errno=%d select=%d errno=%d empty=%d errno=%d duplicates=%d errno=%d\n",n,a,ae,b,be,c,ce,d,de);return a==1&&b==1&&c==0&&d==0?0:1;}
