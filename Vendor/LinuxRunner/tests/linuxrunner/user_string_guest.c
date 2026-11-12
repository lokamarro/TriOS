#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc,char **argv) {
 if(argc>1) {
  char path[1024]="/tmp/";
  for(int i=5;i<1000;i++)path[i]=((i-5)%41==40)?'/':'a';path[1000]=0;
  int iterations=atoi(argv[1]);for(int i=0;i<iterations;i++){errno=0;int fd=open(path,O_RDONLY);assert(fd==-1 && errno==ENOENT);}
  printf("PATH_COPY_BENCH_PASS operations=%d path_bytes=1001\n",iterations);return 0;
 }
 char *p=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);assert(p!=MAP_FAILED);
 assert(!mprotect(p+4096,4096,PROT_NONE));p[4095]=0;errno=0;assert(open(p+4095,O_RDONLY)==-1 && errno==ENOENT);
 p[4095]='x';errno=0;assert(open(p+4095,O_RDONLY)==-1 && errno==EFAULT);
 errno=0;assert(open(p+4096,O_RDONLY)==-1 && errno==EFAULT);
 assert(!mprotect(p+4096,4096,PROT_READ|PROT_WRITE));memcpy(p+4093,"/tmp",5);
 int fd=open(p+4093,O_RDONLY|O_DIRECTORY);assert(fd>=0);assert(!close(fd));
 int zero=open("/dev/zero",O_RDONLY);assert(zero>=0);errno=0;assert(read(zero,(void *)(uintptr_t)(UINT64_MAX-3),8)==-1 && errno==EFAULT);assert(!close(zero));
 assert(!munmap(p,8192));puts("USER_STRING_GUEST_PASS terminator_guard=PASS crossing=PASS faults=PASS");return 0;
}
