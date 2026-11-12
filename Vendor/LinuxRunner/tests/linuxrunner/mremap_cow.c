#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void){size_t n=4096*8;unsigned char *p=mmap(NULL,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)return 2;memset(p,0x31,n);int ready[2],release[2];if(pipe(ready)||pipe(release))return 2;pid_t child=fork();if(child<0)return 2;if(!child){close(ready[0]);close(release[1]);char c='r';if(write(ready[1],&c,1)!=1||read(release[0],&c,1)!=1)_exit(3);for(size_t i=0;i<n;i++)if(p[i]!=0x31)_exit(4);_exit(0);}close(ready[1]);close(release[0]);char c;if(read(ready[0],&c,1)!=1)return 2;p[4096]=0x52;unsigned char *q=mremap(p,n,n+4096*4,MREMAP_MAYMOVE);if(q==MAP_FAILED){perror("mremap after partial COW");return 1;}if(q[4096]!=0x52||q[0]!=0x31||q[n]!=0)return 1;memset(q+n,0x67,4096*4);c='x';if(write(release[1],&c,1)!=1)return 2;int status;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))return 1;munmap(q,n+4096*4);puts("PASS: mremap mixed COW state preserves private writes, child isolation and zero-filled growth");return 0;}
