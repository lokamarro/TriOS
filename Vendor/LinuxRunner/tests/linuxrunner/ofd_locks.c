#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void){char path[100];snprintf(path,sizeof path,"/tmp/ofd-%d",getpid());int a=open(path,O_CREAT|O_RDWR,0600),b=open(path,O_RDWR);CHECK(a>=0&&b>=0);struct flock l={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0x100002000LL,.l_len=4096};CHECK(fcntl(a,F_OFD_SETLK,&l)==0);int duplicate=dup(a);CHECK(duplicate>=0);struct flock q=l;CHECK(fcntl(b,F_OFD_GETLK,&q)==0&&q.l_type==F_WRLCK&&q.l_pid==-1&&q.l_start==l.l_start&&q.l_len==4096);errno=0;CHECK(fcntl(b,F_OFD_SETLK,&l)==-1&&errno==EAGAIN);errno=0;CHECK(fcntl(b,F_SETLK,&l)==-1&&errno==EAGAIN);CHECK(close(a)==0);q=l;CHECK(fcntl(b,F_OFD_GETLK,&q)==0&&q.l_type==F_WRLCK);int pipefd[2];CHECK(pipe(pipefd)==0);pid_t child=fork();CHECK(child>=0);if(!child){close(duplicate);close(pipefd[0]);char c='r';if(write(pipefd[1],&c,1)!=1)_exit(2);if(fcntl(b,F_OFD_SETLKW,&l))_exit(3);l.l_type=F_UNLCK;if(fcntl(b,F_OFD_SETLK,&l))_exit(4);_exit(0);}close(pipefd[1]);char c;CHECK(read(pipefd[0],&c,1)==1);usleep(100000);CHECK(close(duplicate)==0);int status;CHECK(waitpid(child,&status,0)==child&&status==0);q=l;CHECK(fcntl(b,F_OFD_GETLK,&q)==0&&q.l_type==F_UNLCK);CHECK(close(b)==0);unlink(path);puts("PASS: AArch64 64-bit flock layout, OFD conflicts/PID, dup lifetime, fork and blocking close wakeup");return 0;}
