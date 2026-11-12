#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <string.h>
#include <errno.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"PTY_REOPEN_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void){
 int master=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK);CHECK(master>=0);CHECK(grantpt(master)==0&&unlockpt(master)==0);
 char *name=ptsname(master);CHECK(name);char path[128];snprintf(path,sizeof path,"%s",name);
 int slave=open(path,O_RDWR|O_NOCTTY);CHECK(slave>=0);CHECK(close(slave)==0);
 struct pollfd p={master,POLLIN,0};CHECK(poll(&p,1,0)>=0);
 // This initial probing close must not poison the later shell endpoint.
 slave=open(path,O_RDWR|O_NOCTTY);CHECK(slave>=0);struct termios t;CHECK(tcgetattr(slave,&t)==0);cfmakeraw(&t);CHECK(tcsetattr(slave,TCSANOW,&t)==0);
 p.revents=0;CHECK(poll(&p,1,0)>=0);CHECK(!(p.revents&POLLHUP));
 const char message[]="pty-reopen-ok";char b[64]={0};CHECK(write(slave,message,sizeof message)==sizeof message);
 CHECK(poll(&p,1,1000)>0);CHECK(read(master,b,sizeof b)==sizeof message);CHECK(!memcmp(b,message,sizeof message));
 memset(b,0,sizeof b);CHECK(write(master,message,sizeof message)==sizeof message);CHECK(read(slave,b,sizeof b)==sizeof message);CHECK(!memcmp(b,message,sizeof message));
 CHECK(close(slave)==0);p.revents=0;CHECK(poll(&p,1,1000)>0&&(p.revents&POLLHUP));CHECK(close(master)==0);
 puts("PTY_REOPEN_PASS bidirectional I/O restored; final close still hangs up");return 0;
}
