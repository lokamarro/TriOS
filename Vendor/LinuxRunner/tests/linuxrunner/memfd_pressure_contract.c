#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
static unsigned count(const char*name){FILE*f=fopen("/proc/ish/fds","r");assert(f);char line[640];unsigned n=~0U;while(fgets(line,sizeof line,f))if(!strncmp(line,"FD_TREND ",9)){char*p=strstr(line," host_open=");assert(p);n=strtoul(p+11,NULL,10);printf("%s %s",name,line);}fclose(f);assert(n!=~0U);return n;}
int main(void){struct rlimit limit={256,256};assert(!setrlimit(RLIMIT_NOFILE,&limit));unsigned before=count("BEFORE");int fds[256];unsigned n=0;for(;n<256;n++){fds[n]=memfd_create("pressure",MFD_ALLOW_SEALING);if(fds[n]<0){assert(errno==EMFILE);break;}}assert(n>0&&n<256);for(unsigned i=0;i<100;i++){errno=0;int f=memfd_create("failure",MFD_ALLOW_SEALING);assert(f==-1&&errno==EMFILE);}for(unsigned i=0;i<n;i++)close(fds[i]);unsigned after=count("AFTER");assert(after<=before+1);printf("MEMFD_PRESSURE_PASS allocated=%u before=%u after=%u failures=100\n",n,before,after);}
