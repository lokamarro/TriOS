#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned snapshot(const char *tag){FILE*f=fopen("/proc/ish/fds","r");assert(f);char line[512];unsigned descriptions=~0U;while(fgets(line,sizeof line,f))if(!strncmp(line,"FD_TREND ",9)){char*p=strstr(line," descriptions=");assert(p);descriptions=strtoul(p+14,0,10);printf("%s %s",tag,line);}fclose(f);assert(descriptions!=~0U);return descriptions;}
int main(int argc,char **argv){if(argc==2)return 0;unsigned before=snapshot("BEFORE");for(int n=0;n<32;n++){pid_t p=fork();assert(p>=0);if(!p){for(int j=0;j<24;j++){int f=open("/dev/null",O_RDONLY);assert(f>=0);assert(dup(f)>=0);}execl(argv[0],argv[0],"child",NULL);_exit(99);}int status;assert(waitpid(p,&status,0)==p&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}unsigned after=snapshot("AFTER_FORK_EXEC");assert(after<=before+2);char name[]="/tmp/lr-map-reference-XXXXXX";int fd=mkstemp(name);assert(fd>=0&&!ftruncate(fd,4096));void*map=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(map!=MAP_FAILED);close(fd);unlink(name);*(volatile int*)map=42;snapshot("LIVE_MAPPING");assert(!munmap(map,4096));after=snapshot("AFTER_UNMAP");assert(after<=before+2);puts("FD_CENSUS_LIFECYCLE PASS fork/exec inherited descriptors and closed-file mapping references return to baseline");return 0;}
