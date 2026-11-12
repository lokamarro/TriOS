#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <unistd.h>
static atomic_uint checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"NESTED_EPOLL_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void add(int ep,int fd,unsigned flags,uint64_t data){struct epoll_event e={.events=flags,.data.u64=data};
 CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e)==0);}
static void mod(int ep,int fd,unsigned flags,uint64_t data){struct epoll_event e={.events=flags,.data.u64=data};
 CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,fd,&e)==0);}
static int ready(int ep){struct pollfd p={.fd=ep,.events=POLLIN|POLLOUT};
 CHECK(poll(&p,1,0)>=0);
 CHECK(!(p.revents&POLLOUT));return !!(p.revents&POLLIN);}
static void *writer(void *arg){usleep(30000);int fd=*(int*)arg;
 CHECK(write(fd,"X",1)==1);return NULL;}
struct close_args { int child, parent; };
static void *close_child(void *arg){struct close_args *a=arg;usleep(10000);CHECK(close(a->child)==0);return NULL;}
static void additional_contracts(void){
 // More host events than the nonblocking backend observation batch; no ET
 // event may be lost or re-delivered while the underlying pipe stays readable.
 int ep=epoll_create1(0),fds[96][2];CHECK(ep>=0);
 for(unsigned i=0;i<96;i++){CHECK(pipe(fds[i])==0);add(ep,fds[i][0],EPOLLIN|EPOLLET,i);CHECK(write(fds[i][1],"x",1)==1);}
 unsigned seen[96]={0},total=0;struct epoll_event events[17];
 for(unsigned pass=0;pass<20;pass++){int n=epoll_wait(ep,events,17,0);CHECK(n>=0);if(!n)break;for(int j=0;j<n;j++){CHECK(events[j].data.u64<96);CHECK(!seen[events[j].data.u64]++);total++;}}
 CHECK(total==96);CHECK(epoll_wait(ep,events,17,0)==0);
 for(unsigned i=0;i<96;i++){close(fds[i][0]);close(fds[i][1]);}close(ep);
 // Aliases retain separate event data and ONESHOT state; inspecting the
 // containing set must not consume either registration.
 int parent=epoll_create1(0),child=epoll_create1(0),leaf=eventfd(1,EFD_NONBLOCK),alias=dup(leaf);CHECK(parent>=0&&child>=0&&leaf>=0&&alias>=0);
 add(child,leaf,EPOLLIN|EPOLLONESHOT,101);add(child,alias,EPOLLIN|EPOLLONESHOT,102);add(parent,child,EPOLLIN,103);
 CHECK(ready(parent)&&ready(parent));int n=epoll_wait(child,events,17,0);CHECK(n==2);CHECK(events[0].data.u64!=events[1].data.u64);CHECK(!ready(parent));
 mod(child,alias,EPOLLIN|EPOLLONESHOT,104);CHECK(ready(parent));CHECK(epoll_wait(child,events,17,0)==1&&events[0].data.u64==104);CHECK(!ready(parent));
 close(leaf);close(alias);close(child);close(parent);
 // Closing a child while the parent is waiting removes its registration
 // without accessing freed backend event pointers.
 for(int i=0;i<40;i++){struct close_args a={epoll_create1(0),epoll_create1(0)};CHECK(a.child>=0&&a.parent>=0);add(a.parent,a.child,EPOLLIN,9);pthread_t t;CHECK(pthread_create(&t,NULL,close_child,&a)==0);CHECK(epoll_wait(a.parent,events,17,25)==0);CHECK(pthread_join(t,NULL)==0);CHECK(!ready(a.parent));close(a.parent);}
 // Multiple ancestors and a diamond must not permit a cycle or deadlock.
 int a=epoll_create1(0),b=epoll_create1(0),c=epoll_create1(0),d=epoll_create1(0);CHECK(a>=0&&b>=0&&c>=0&&d>=0);add(a,b,EPOLLIN,1);add(a,c,EPOLLIN,2);add(b,d,EPOLLIN,3);add(c,d,EPOLLIN,4);struct epoll_event ev={.events=EPOLLIN};errno=0;CHECK(epoll_ctl(d,EPOLL_CTL_ADD,a,&ev)==-1&&errno==ELOOP);close(a);close(b);close(c);close(d);
}
int main(void){alarm(20);int child=epoll_create1(EPOLL_CLOEXEC),parent=epoll_create1(0),leaf=eventfd(0,EFD_NONBLOCK);
 CHECK(child>=0&&parent>=0&&leaf>=0);add(child,leaf,EPOLLIN,11);add(parent,child,EPOLLIN,22);
 CHECK(!ready(child)&&!ready(parent));uint64_t v=1;
 CHECK(write(leaf,&v,8)==8);
 CHECK(ready(child));
 CHECK(ready(child)&&ready(parent));fd_set set;FD_ZERO(&set);FD_SET(parent,&set);struct timeval zero={0};
 CHECK(select(parent+1,&set,NULL,NULL,&zero)==1&&FD_ISSET(parent,&set));struct epoll_event e;
 CHECK(epoll_wait(parent,&e,1,100)==1&&e.data.u64==22);
 CHECK(epoll_wait(child,&e,1,0)==1&&e.data.u64==11);
 CHECK(ready(parent));
 CHECK(read(leaf,&v,8)==8);
 CHECK(!ready(child)&&!ready(parent));
 mod(child,leaf,EPOLLIN|EPOLLET|EPOLLONESHOT,33);v=1;
 CHECK(write(leaf,&v,8)==8);
 CHECK(ready(parent)&&ready(child));
 CHECK(epoll_wait(child,&e,1,0)==1&&e.data.u64==33);
 CHECK(!ready(parent));
 mod(child,leaf,EPOLLIN|EPOLLET|EPOLLONESHOT,44);
 CHECK(ready(parent));
 CHECK(epoll_wait(child,&e,1,0)==1&&e.data.u64==44);
 CHECK(read(leaf,&v,8)==8);
 CHECK(!ready(parent));
 close(leaf);
 int p[2];
 CHECK(pipe(p)==0);add(child,p[0],EPOLLIN|EPOLLET,55);pthread_t t;
 CHECK(pthread_create(&t,NULL,writer,&p[1])==0);
 CHECK(epoll_wait(parent,&e,1,500)==1&&e.data.u64==22);
 CHECK(pthread_join(t,NULL)==0);
 CHECK(ready(child)&&ready(child));
 CHECK(epoll_wait(child,&e,1,0)==1&&e.data.u64==55);
 CHECK(!ready(parent));char c;
 CHECK(read(p[0],&c,1)==1&&c=='X');
 CHECK(pthread_create(&t,NULL,writer,&p[1])==0);
 CHECK(epoll_wait(parent,&e,1,500)==1&&e.data.u64==22);
 CHECK(epoll_wait(child,&e,1,0)==1&&e.data.u64==55);
 CHECK(read(p[0],&c,1)==1);
 CHECK(pthread_join(t,NULL)==0);
 CHECK(!ready(parent));
 struct epoll_event ev={.events=EPOLLIN};errno=0;
 CHECK(epoll_ctl(child,EPOLL_CTL_ADD,parent,&ev)==-1&&errno==ELOOP);errno=0;
 CHECK(epoll_ctl(child,EPOLL_CTL_ADD,child,&ev)==-1&&errno==EINVAL);
 close(child);
 CHECK(!ready(parent));
 close(parent);
 close(p[0]);
 close(p[1]);
 int chain[6];for(int i=0;i<6;i++){chain[i]=epoll_create1(0);
 CHECK(chain[i]>=0);}for(int i=0;i<4;i++)add(chain[i],chain[i+1],EPOLLIN,i);errno=0;
 CHECK(epoll_ctl(chain[4],EPOLL_CTL_ADD,chain[5],&ev)==-1&&errno==ELOOP);for(int i=0;i<6;i++)close(chain[i]);
 additional_contracts();
 puts("NESTED_EPOLL_PASS poll select nonconsuming LT ET oneshot delayed_pipe cycle depth close");printf("checks=%u\n",atomic_load(&checks));return 0;}
