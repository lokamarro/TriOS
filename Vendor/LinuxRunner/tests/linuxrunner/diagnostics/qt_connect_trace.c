// Test-only LD_PRELOAD observer: no packet/authentication payloads are logged.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int connect(int fd,const struct sockaddr *a,socklen_t n){
 int (*real_connect)(int,const struct sockaddr*,socklen_t)=dlsym(RTLD_NEXT,"connect");
 int result=real_connect(fd,a,n),saved=errno;
 if(a&&a->sa_family==AF_UNIX&&n>2){
  const struct sockaddr_un*u=(const void*)a;const char*p=u->sun_path;if(!*p)p++;
  if(!strncmp(p,"/tmp/.X11-unix/X",15))fprintf(stderr,"QT_CONNECT_TRACE target=%.*s result=%d errno=%d\n",(int)(n-2-(!u->sun_path[0])),p,result,result<0?saved:0);
 }
 errno=saved;return result;
}
