#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
/* Guest executable data is interpreted. This never requests executable host
 * memory or JIT. STR W exercises the optimized store and its code invalidation.
 */
static void put(void *address,uint32_t word) {
 __asm__ volatile("str %w0, [%1, #0]"::"r"(word),"r"(address):"memory");
}
static int patch_read_call(uint32_t *code, void *cold, uint32_t word) {
 int result;
 // One basic block: a code write, a cold unrelated read, then the indirect
 // call. The read miss must not replace the pending code-write marker.
 __asm__ volatile("str %w3, [%1]\nldr w9, [%2]\nblr %1\nmov %w0, w0"
                  : "=r"(result) : "r"(code), "r"(cold), "r"(word)
                  : "x0", "x9", "x30", "memory");
 return result;
}
int main(void) {
 alarm(30);
 uint32_t *page=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(page==MAP_FAILED)return 1;
 put(page,0x52800020);put(page+1,0xd65f03c0); /* mov w0,1; ret */
 int (*f)(void)=(int(*)(void))page;
 for(int i=0;i<100;i++)if(f()!=1)return 2;
 put(page,0x52800040); /* mov w0,2; ret; re-use a decoded/chained block */
 for(int i=0;i<100;i++)if(f()!=2)return 3;
 if(mprotect(page,4096,PROT_READ|PROT_EXEC))return 4;
 if(mprotect(page,4096,PROT_READ|PROT_WRITE|PROT_EXEC))return 5;
 put(page,0x52800060);if(f()!=3)return 6;
 void *cold=mmap(NULL,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(cold==MAP_FAILED)return 12;
 for(int i=0;i<100;i++)if(f()!=3)return 13; /* warm decoded code after mapping */
 if(patch_read_call(page,cold,0x528000a0)!=5)return 14;
 put(page,0x52800060);if(f()!=3)return 15;
 if(munmap(cold,4096))return 16;
 pid_t pid=fork();if(pid<0)return 7;
 if(!pid){put(page,0x52800080);_exit(f()==4?0:8);}
 int status;if(waitpid(pid,&status,0)!=pid||!WIFEXITED(status)||WEXITSTATUS(status))return 9;
 if(f()!=3)return 10; /* child code patch must not escape copy-on-write */
 if(munmap(page,4096))return 11;
 puts("RUNTIME_CODE_COHERENCE_PASS self-modification,cold-read-after-write,mprotect,fork-COW");return 0;
}
