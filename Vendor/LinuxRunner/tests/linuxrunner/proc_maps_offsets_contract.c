#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
static void check(const char *label, void *address, unsigned long long expected_offset, char sharing, int file_backed) {
    FILE *file=fopen("/proc/self/maps","r");
    if(!file){perror("maps");exit(2);}
    char row[8192],permissions[5]; unsigned long long begin,end,offset;int found=0;
    while(fgets(row,sizeof row,file)) {
        if(sscanf(row,"%llx-%llx %4s %llx",&begin,&end,permissions,&offset)!=4)continue;
        if((uintptr_t)address<begin || (uintptr_t)address>=end)continue;
        found=1;
        // A VMA can include the tested page within a larger unbroken region.
        unsigned long long effective=offset+(file_backed?(uintptr_t)address-begin:0);
        int ok=effective==expected_offset && permissions[3]==sharing;
        printf("%s %s offset=%#llx expected=%#llx sharing=%c expected=%c\n",label,ok?"PASS":"FAIL",effective,expected_offset,permissions[3],sharing);
        if(!ok)failures++;
        break;
    }
    fclose(file);if(!found){printf("%s FAIL mapping missing\n",label);failures++;}
}
int main(void) {
    char name[]="/tmp/lr-proc-maps-XXXXXX";
    int fd=mkstemp(name);if(fd<0||ftruncate(fd,65536)){perror("file");return 2;}
    unsigned char *map=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_SHARED,fd,4096);
    if(map==MAP_FAILED){perror("mmap");return 2;}
    check("shared initial guest-aligned offset",map,4096,'s',1);
    if(mprotect(map+4096,4096,PROT_READ)){perror("protect");return 2;}
    check("shared protection split middle",map+4096,8192,'s',1);
    check("shared protection split tail",map+8192,12288,'s',1);
    unsigned char *private_fork=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,32768);
    if(private_fork==MAP_FAILED)return 2;
    fflush(stdout);pid_t child=fork();if(child<0)return 2;
    if(!child){
        check("fork retained split offset",map+8192,12288,'s',1);
        private_fork[4096]=0x5a;
        check("private CoW retained file offset",private_fork+4096,36864,'p',1);
        if(munmap(private_fork,8192))_exit(2);
        fflush(stdout);_exit(failures?1:0);
    }
    int status;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))failures++;
    if(private_fork[4096]!=0){puts("private CoW FAIL parent changed");failures++;}
    if(munmap(private_fork,8192))return 2;
    if(munmap(map,4096))return 2;
    check("unmap retained split tail",map+8192,12288,'s',1);
    if(munmap(map+4096,8192))return 2;
    map=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,16384);
    if(map==MAP_FAILED||mprotect(map+4096,4096,PROT_READ))return 2;
    check("private protection split middle",map+4096,20480,'p',1);
    check("private protection split tail",map+8192,24576,'p',1);
    if(munmap(map,12288))return 2;
    // Adjacent private/shared anonymous VMAs with the same RW permissions
    // must not be merged into one private row by the procfs formatter.
    map=mmap(NULL,8192,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(map==MAP_FAILED)return 2;
    if(mmap(map,4096,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)!=map)return 2;
    if(mmap(map+4096,4096,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_SHARED|MAP_ANONYMOUS,-1,0)!=map+4096)return 2;
    check("adjacent private anonymous",map,0,'p',0);
    check("adjacent shared anonymous",map+4096,0,'s',0);
    if(munmap(map,8192))return 2;
    close(fd);unlink(name);
    printf("PROC_MAPS_OFFSETS %s failures=%d\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
