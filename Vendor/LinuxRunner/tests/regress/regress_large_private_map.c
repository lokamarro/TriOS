#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>
#define SIZE (512UL*1024*1024)
int main(void){
 struct sysinfo before,after;sysinfo(&before);
 volatile unsigned char *p=mmap(0,SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(p==MAP_FAILED)return 1;
 sysinfo(&after);
 if(before.freeram-after.freeram>8192){fprintf(stderr,"untouched private mapping charged eagerly: %lu bytes\n",before.freeram-after.freeram);return 13;}
 if(p[0]||p[SIZE-1])return 2;p[0]=11;p[SIZE-1]=22;
 pid_t child=fork();if(child<0)return 3;
 if(!child){if(p[0]!=11||p[SIZE-1]!=22||p[SIZE/2]!=0)_exit(4);p[0]=33;p[SIZE/2]=44;_exit(0);}
 int status;if(waitpid(child,&status,0)!=child||status)return 5;
 if(p[0]!=11||p[SIZE/2]!=0)return 6;
 if(mprotect((void *)(p+SIZE/2),4096,PROT_READ))return 7;
 if(mprotect((void *)(p+SIZE/2),4096,PROT_READ|PROT_WRITE))return 8;
 p[SIZE/2]=55;
 if(mmap((void *)p,SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=(void *)p)return 9;
 if(p[0]||p[SIZE/2]||p[SIZE-1])return 10;
 if(munmap((void *)p,SIZE))return 11;
 sysinfo(&after);
 if(after.freeram!=before.freeram){fprintf(stderr,"memory accounting drift: %lu -> %lu\n",before.freeram,after.freeram);return 12;}
 puts("LARGE_PRIVATE_MAP_PASS demand zero, fork CoW, untouched fork pages, mprotect, MAP_FIXED replacement, unmap accounting");return 0;
}
