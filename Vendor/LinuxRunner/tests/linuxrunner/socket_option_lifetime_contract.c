#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
static _Atomic int shared=-1,stop;
static void *query(void *p){(void)p;while(!atomic_load(&stop)){int fd=atomic_load(&shared);if(fd<0)continue;int value=0;socklen_t length=sizeof value;int r=getsockopt(fd,SOL_SOCKET,SO_TYPE,&value,&length);assert(r==0||errno==EBADF||errno==ENOTSOCK);int enabled=1;r=setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&enabled,sizeof enabled);assert(r==0||errno==EBADF||errno==ENOTSOCK);}return 0;}
int main(void){pthread_t t;assert(!pthread_create(&t,0,query,0));for(int i=0;i<4000;i++){int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);atomic_store(&shared,fd);if(i%20==0)usleep(100);assert(!close(fd));atomic_store(&shared,-1);}atomic_store(&stop,1);assert(!pthread_join(t,0));int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);for(int i=0;i<500;i++){errno=0;assert(setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,(void *)1,4)==-1&&errno==EFAULT);errno=0;assert(getsockopt(fd,SOL_SOCKET,SO_TYPE,(void *)1,(void *)1)==-1&&errno==EFAULT);}close(fd);puts("SOCKET_OPTION_LIFETIME PASS concurrent close/reuse and 1000 EFAULT cleanups");return 0;}
