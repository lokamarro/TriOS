#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
static int failures;
static void one(int anonymous,off_t offset) {
 char path[]="/tmp/lr-partial-XXXXXX",link[80];
 int fd=anonymous?memfd_create("partial-page",MFD_ALLOW_SEALING):mkstemp(path);
 if(fd<0||ftruncate(fd,offset+123)){perror("create/size");exit(2);}
 snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
 int ro=open(anonymous?link:path,O_RDONLY);if(ro<0){perror("reopen");exit(2);}if(!anonymous)unlink(path);
 volatile unsigned char*r=mmap(NULL,4096,PROT_READ,MAP_SHARED,ro,offset);
 volatile unsigned char*w=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,offset);
 if(r==MAP_FAILED||w==MAP_FAILED){perror("mmap");exit(2);}
 w[0]=37;int coherent=(r[0]==37);unsigned char byte=61;
 if(pwrite(fd,&byte,1,offset)!=1){perror("pwrite");exit(2);}
 coherent=coherent&&r[0]==61&&w[0]==61;
 errno=0;int permissions=mprotect((void*)r,4096,PROT_READ|PROT_WRITE)==-1&&errno==EACCES;
 close(ro);close(fd);int lifetime=r[0]==61&&w[0]==61;
 munmap((void*)r,4096);munmap((void*)w,4096);
 printf("PARTIAL_SHARED kind=%s offset=%ld coherent=%d readonly=%d lifetime=%d\n",anonymous?"memfd":"regular",(long)offset,coherent,permissions,lifetime);
 failures+=!coherent||!permissions||!lifetime;
}
int main(void){for(int a=0;a<2;a++){one(a,0);one(a,4096);one(a,16384);}printf("PARTIAL_SHARED_RESULT failures=%d\n",failures);return failures?1:0;}
