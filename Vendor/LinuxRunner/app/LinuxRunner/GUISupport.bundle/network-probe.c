/* Read-only network compatibility probe. No host network configuration changes. */
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void){
    unsigned failed=0,interfaces=0,addresses=0;struct ifaddrs *all=NULL;
    if(getifaddrs(&all)){printf("NETWORK: getifaddrs failed errno=%d\n",errno);failed++;}
    else{for(struct ifaddrs*p=all;p;p=p->ifa_next){if(!p->ifa_name)continue;interfaces++;if(p->ifa_addr&&(p->ifa_addr->sa_family==AF_INET||p->ifa_addr->sa_family==AF_INET6))addresses++;}freeifaddrs(all);printf("NETWORK: getifaddrs entries=%u IP-addresses=%u\n",interfaces,addresses);if(!interfaces||!addresses)failed++;}
    struct if_nameindex *names=if_nameindex();
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(!names||fd<0){printf("NETWORK: nameindex/socket failure errno=%d\n",errno);failed++;}
    else for(struct if_nameindex*p=names;p->if_index;p++){
        struct ifreq q={0};snprintf(q.ifr_name,sizeof q.ifr_name,"%s",p->if_name);
        if(ioctl(fd,SIOCGIFINDEX,&q)||q.ifr_ifindex!=(int)p->if_index){printf("NETWORK: index failed %s errno=%d\n",p->if_name,errno);failed++;}
        memset(&q,0,sizeof q);q.ifr_ifindex=p->if_index;
        if(ioctl(fd,SIOCGIFNAME,&q)||strcmp(q.ifr_name,p->if_name)){printf("NETWORK: name failed index=%u errno=%d\n",p->if_index,errno);failed++;}
        memset(&q,0,sizeof q);snprintf(q.ifr_name,sizeof q.ifr_name,"%s",p->if_name);
        if(ioctl(fd,SIOCGIFFLAGS,&q)){printf("NETWORK: flags failed %s errno=%d\n",p->if_name,errno);failed++;}
        unsigned flags=(unsigned short)q.ifr_flags;
        if(ioctl(fd,SIOCGIFMTU,&q)){printf("NETWORK: MTU failed %s errno=%d\n",p->if_name,errno);failed++;}
        printf("NETWORK: interface=%s index=%u Linux-flags=%x MTU=%d (host-owned read-only view)\n",p->if_name,p->if_index,flags,q.ifr_mtu);
    }
    if(names)if_freenameindex(names);
    if(fd>=0){struct ifconf c={0};if(ioctl(fd,SIOCGIFCONF,&c)||c.ifc_len<=0){printf("NETWORK: IFCONF sizing failed errno=%d bytes=%d\n",errno,c.ifc_len);failed++;}
        struct ifreq entries[64];c.ifc_len=sizeof entries;c.ifc_req=entries;
        if(ioctl(fd,SIOCGIFCONF,&c)||c.ifc_len<=0){printf("NETWORK: IFCONF data failed errno=%d\n",errno);failed++;}else printf("NETWORK: IFCONF IPv4-entries=%zu\n",c.ifc_len/sizeof(struct ifreq));close(fd);}
    struct addrinfo *resolved=NULL;int dns=getaddrinfo("dl-cdn.alpinelinux.org","443",NULL,&resolved);printf("NETWORK: DNS=%s\n",dns?gai_strerror(dns):"OK");if(dns)failed++;if(resolved)freeaddrinfo(resolved);
    printf("NETWORK: %s failures=%u\n",failed?"FAIL":"PASS",failed);return failed?1:0;
}
