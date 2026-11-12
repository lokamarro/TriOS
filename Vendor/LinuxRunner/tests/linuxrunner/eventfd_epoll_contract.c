#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"EVENT_CONTRACT_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define ERROR(call,e) do{errno=0;CHECK((call)==-1&&errno==(e));}while(0)
int main(void){
 int e=eventfd(3,EFD_SEMAPHORE|EFD_NONBLOCK|EFD_CLOEXEC);CHECK(e>=0);
 int dupfd=dup(e);CHECK(dupfd>=0);uint64_t v;
 CHECK(read(e,&v,8)==8&&v==1);CHECK(read(dupfd,&v,8)==8&&v==1);CHECK(read(e,&v,8)==8&&v==1);ERROR(read(e,&v,8),EAGAIN);
 v=4;CHECK(write(e,&v,8)==8);CHECK(read(e,&v,8)==8&&v==1);
 int p=epoll_create1(EPOLL_CLOEXEC);CHECK(p>=0);struct epoll_event ev={.events=EPOLLIN,.data.u64=123},out[128];
 CHECK(epoll_ctl(p,EPOLL_CTL_ADD,e,&ev)==0);CHECK(epoll_wait(p,out,134217727,0)==1&&out[0].data.u64==123);ERROR(epoll_wait(p,out,134217728,0),EINVAL);
 ERROR(epoll_ctl(p,123,e,&ev),EINVAL);ERROR(epoll_ctl(p,EPOLL_CTL_ADD,p,&ev),EINVAL);
 while(read(e,&v,8)==8)CHECK(v==1);CHECK(errno==EAGAIN);CHECK(epoll_wait(p,out,128,0)==0);
 int fds[140];for(int i=0;i<140;i++){fds[i]=eventfd(1,EFD_NONBLOCK);CHECK(fds[i]>=0);ev.data.u64=1000+i;CHECK(epoll_ctl(p,EPOLL_CTL_ADD,fds[i],&ev)==0);}
 unsigned seen=0;for(int pass=0;pass<3;pass++){int n=epoll_wait(p,out,140,0);CHECK(n>=0&&n<=128);for(int j=0;j<n;j++){int i=out[j].data.u64-1000;CHECK(i>=0&&i<140);CHECK(read(fds[i],&v,8)==8&&v==1);seen++;}}
 CHECK(seen==140);for(int i=0;i<140;i++)CHECK(close(fds[i])==0);
 for(int mode=0;mode<2;mode++){
  unsigned char ids[140]={0};seen=0;
  for(int i=0;i<140;i++){fds[i]=eventfd(1,EFD_NONBLOCK);CHECK(fds[i]>=0);ev.events=EPOLLIN|(mode?EPOLLET:EPOLLONESHOT);ev.data.u64=2000+i;CHECK(epoll_ctl(p,EPOLL_CTL_ADD,fds[i],&ev)==0);}
  for(int pass=0;pass<3;pass++){int n=epoll_wait(p,out,140,0);CHECK(n>=0&&n<=128);for(int j=0;j<n;j++){int i=out[j].data.u64-2000;CHECK(i>=0&&i<140&&!ids[i]);ids[i]=1;seen++;}}
  CHECK(seen==140);
  for(int i=0;i<140;i++){CHECK(read(fds[i],&v,8)==8&&v==1);CHECK(close(fds[i])==0);}
 }
 unsigned char persistent[140]={0};seen=0;
 for(int i=0;i<140;i++){fds[i]=eventfd(1,EFD_NONBLOCK);CHECK(fds[i]>=0);ev.events=EPOLLIN;ev.data.u64=3000+i;CHECK(epoll_ctl(p,EPOLL_CTL_ADD,fds[i],&ev)==0);}
 for(int pass=0;pass<3;pass++){int n=epoll_wait(p,out,140,0);CHECK(n>0&&n<=128);for(int j=0;j<n;j++){int i=out[j].data.u64-3000;CHECK(i>=0&&i<140);if(!persistent[i]){persistent[i]=1;seen++;}}}
 CHECK(seen==140);for(int i=0;i<140;i++)CHECK(close(fds[i])==0);
 CHECK(close(p)==0&&close(e)==0&&close(dupfd)==0);
 puts("EVENTFD_EPOLL_CONTRACT_PASS semaphore shared_description readiness bounded_batch oneshot edge_triggered fair_level_readiness invalid_args");return 0;
}
