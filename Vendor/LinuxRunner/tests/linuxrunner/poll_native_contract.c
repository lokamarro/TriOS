#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static pthread_t main_thread;static int peer;static void handler(int n){(void)n;}
static void *interrupt(void*x){(void)x;usleep(20000);assert(!pthread_kill(main_thread,SIGUSR1));return 0;}
static void *write_peer(void*x){(void)x;usleep(20000);assert(write(peer,"x",1)==1);return 0;}
int main(void){int s[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,s));peer=s[1];main_thread=pthread_self();struct sigaction a={.sa_handler=handler};sigemptyset(&a.sa_mask);assert(!sigaction(SIGUSR1,&a,0));pthread_t t;assert(!pthread_create(&t,0,interrupt,0));struct pollfd p={.fd=s[0],.events=POLLIN};errno=0;assert(poll(&p,1,1000)==-1&&errno==EINTR);assert(!pthread_join(t,0));assert(!pthread_create(&t,0,write_peer,0));assert(poll(&p,1,1000)==1&&(p.revents&POLLIN));assert(!pthread_join(t,0));char b;assert(read(s[0],&b,1)==1);struct pollfd n[2]={{.fd=-1,.events=POLLIN},{.fd=4095,.events=POLLIN}};assert(poll(n,2,0)==1&&n[0].revents==0&&(n[1].revents&POLLNVAL));close(s[1]);assert(poll(&p,1,1000)==1&&(p.revents&POLLIN));fd_set r,w;FD_ZERO(&r);FD_ZERO(&w);FD_SET(s[0],&r);struct timeval zero={0};assert(select(s[0]+1,&r,&w,0,&zero)==1&&FD_ISSET(s[0],&r)&&!FD_ISSET(s[0],&w));close(s[0]);printf("POLL_NATIVE PASS signal EINTR, delayed readiness, negative/invalid FD, half-close and select requested-set preservation\n");return 0;}
