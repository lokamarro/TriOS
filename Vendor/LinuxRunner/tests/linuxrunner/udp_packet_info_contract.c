#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static void run(int family) {
 int s=socket(family,SOCK_DGRAM|SOCK_CLOEXEC,0),r=socket(family,SOCK_DGRAM|SOCK_CLOEXEC,0),one=1;assert(s>=0&&r>=0);
 int level=family==AF_INET?IPPROTO_IP:IPPROTO_IPV6;
 int packet_option=family==AF_INET?IP_PKTINFO:IPV6_RECVPKTINFO;
 assert(!setsockopt(r,level,packet_option,&one,sizeof one));
 assert(!setsockopt(r,level,family==AF_INET?IP_RECVTOS:IPV6_RECVTCLASS,&one,sizeof one));
 struct sockaddr_storage addr={0};socklen_t alen;
 if(family==AF_INET){struct sockaddr_in *a=(void *)&addr;a->sin_family=family;a->sin_addr.s_addr=htonl(INADDR_LOOPBACK);alen=sizeof *a;}
 else{struct sockaddr_in6 *a=(void *)&addr;a->sin6_family=family;a->sin6_addr=in6addr_loopback;alen=sizeof *a;}
 assert(!bind(r,(void *)&addr,alen));assert(!getsockname(r,(void *)&addr,&alen));
 union{struct cmsghdr align;char bytes[256];} control={0};
 char byte='p';struct iovec iov={&byte,1};struct msghdr out={.msg_name=&addr,.msg_namelen=alen,.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes};
 struct cmsghdr *c=(void *)control.bytes;c->cmsg_level=level;c->cmsg_type=family==AF_INET?IP_PKTINFO:IPV6_PKTINFO;
 size_t payload=family==AF_INET?sizeof(struct in_pktinfo):sizeof(struct in6_pktinfo);c->cmsg_len=CMSG_LEN(payload);out.msg_controllen=CMSG_SPACE(payload);
 if(family==AF_INET)((struct in_pktinfo *)CMSG_DATA(c))->ipi_spec_dst.s_addr=htonl(INADDR_LOOPBACK);
 else ((struct in6_pktinfo *)CMSG_DATA(c))->ipi6_addr=in6addr_loopback;
 c=(void *)(control.bytes+out.msg_controllen);c->cmsg_level=level;c->cmsg_type=family==AF_INET?IP_TOS:IPV6_TCLASS;c->cmsg_len=CMSG_LEN(sizeof(int));int tos=2;memcpy(CMSG_DATA(c),&tos,sizeof tos);out.msg_controllen+=CMSG_SPACE(sizeof(int));
 assert(sendmsg(s,&out,0)==1);
 struct pollfd p={.fd=r,.events=POLLIN};assert(poll(&p,1,1000)==1);
 memset(&control,0,sizeof control);byte=0;struct msghdr in={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof control.bytes};
 assert(recvmsg(r,&in,0)==1&&byte=='p');assert(!(in.msg_flags&MSG_CTRUNC));int packet=0,traffic=0;
 for(c=CMSG_FIRSTHDR(&in);c;c=CMSG_NXTHDR(&in,c)){
  if(c->cmsg_level!=level)continue;
  if(c->cmsg_type==(family==AF_INET?IP_PKTINFO:IPV6_PKTINFO)){
   packet++;assert(c->cmsg_len==CMSG_LEN(payload));
   if(family==AF_INET){struct in_pktinfo *v=(void *)CMSG_DATA(c);assert(v->ipi_ifindex>0&&v->ipi_addr.s_addr==htonl(INADDR_LOOPBACK));}
   else {struct in6_pktinfo *v=(void *)CMSG_DATA(c);assert(v->ipi6_ifindex>0&&!memcmp(&v->ipi6_addr,&in6addr_loopback,16));}
  }
  if(c->cmsg_type==(family==AF_INET?IP_TOS:IPV6_TCLASS)){int value=0;memcpy(&value,CMSG_DATA(c),c->cmsg_len-CMSG_LEN(0));assert(value==2);traffic++;}
 }
 assert(packet==1&&traffic==1);
 assert(sendto(s,"t",1,0,(void *)&addr,alen)==1);in.msg_controllen=sizeof(struct cmsghdr);in.msg_flags=0;
 assert(recvmsg(r,&in,0)==1&&(in.msg_flags&MSG_CTRUNC));
 errno=0;assert(setsockopt(s,level,0x7fffffff,&one,sizeof one)==-1&&errno==ENOPROTOOPT);
 close(s);close(r);printf("UDP_PACKET_INFO family=%s pktinfo/ECN/sendmsg/recvmsg/truncation/unknown-option PASS\n",family==AF_INET?"IPv4":"IPv6");
}
int main(void){run(AF_INET);run(AF_INET6);return 0;}
