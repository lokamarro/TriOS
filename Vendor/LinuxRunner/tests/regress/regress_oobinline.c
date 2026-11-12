#define _GNU_SOURCE
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static struct sockaddr_in address;static atomic_int ready;
static void *sender(void *p){(void)p;int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);assert(!connect(fd,(void*)&address,sizeof(address)));while(!atomic_load(&ready))usleep(1000);assert(send(fd,"A",1,0)==1);assert(send(fd,"B",1,MSG_OOB)==1);assert(send(fd,"C",1,0)==1);close(fd);return 0;}
int main(void){alarm(20);int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);address=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};assert(!bind(listener,(void*)&address,sizeof(address)));socklen_t len=sizeof(address);assert(!getsockname(listener,(void*)&address,&len));assert(!listen(listener,1));pthread_t t;assert(!pthread_create(&t,0,sender,0));int fd=accept(listener,0,0);assert(fd>=0);int one=1,value=-1;assert(!setsockopt(fd,SOL_SOCKET,SO_OOBINLINE,&one,sizeof(one)));len=sizeof(value);assert(!getsockopt(fd,SOL_SOCKET,SO_OOBINLINE,&value,&len)&&value==1);atomic_store(&ready,1);char buf[4]={0};size_t used=0;while(used<3){ssize_t n=recv(fd,buf+used,3-used,0);assert(n>0);used+=n;}assert(!strcmp(buf,"ABC"));one=0;assert(!setsockopt(fd,SOL_SOCKET,SO_OOBINLINE,&one,sizeof(one)));len=sizeof(value);assert(!getsockopt(fd,SOL_SOCKET,SO_OOBINLINE,&value,&len)&&value==0);close(fd);close(listener);assert(!pthread_join(t,0));puts("OOBINLINE_PASS actual_socket_option_and_inline_urgent_tcp_data");}
