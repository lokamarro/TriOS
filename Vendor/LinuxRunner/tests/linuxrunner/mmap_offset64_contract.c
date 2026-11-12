#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
static int fd=-1;
static char path[]="/tmp/lr-mmap-offset64-XXXXXX";
static void cleanup(void) { if(fd>=0)close(fd);unlink(path); }
#define CHECK(x) do { checks++; if(!(x)) { fprintf(stderr,"MMAP_OFFSET64_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1); } } while(0)
static void permissions(off_t offset,size_t page) {
    int ro=open(path,O_RDONLY),wo=open(path,O_WRONLY);CHECK(ro>=0&&wo>=0);
    errno=0;CHECK((void*)syscall(SYS_mmap,NULL,page,PROT_WRITE,MAP_SHARED,ro,offset)==MAP_FAILED&&errno==EACCES);
    // Extending past EOF must not turn a denied shared mapping into an anonymous
    // writable copy in the host page-alignment fallback.
    errno=0;CHECK((void*)syscall(SYS_mmap,NULL,4*page,PROT_WRITE,MAP_SHARED,ro,offset)==MAP_FAILED&&errno==EACCES);
    errno=0;CHECK((void*)syscall(SYS_mmap,NULL,page,PROT_READ,MAP_PRIVATE,wo,offset)==MAP_FAILED&&errno==EACCES);
    unsigned char *p=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE,ro,offset);CHECK(p!=MAP_FAILED&&p[0]=='H');p[0]='R';CHECK(munmap(p,page)==0);
    unsigned char c;CHECK(pread(ro,&c,1,offset)==1&&c=='H');CHECK(close(ro)==0&&close(wo)==0);
}
int main(int argc,char **argv) {

    alarm(20);atexit(cleanup);
    const size_t page=4096;
    const off_t high=((off_t)1<<32)+page;
    unsigned char bytes[4096];
    fd=mkstemp(path);CHECK(fd>=0);
    CHECK(ftruncate(fd,high+2*page)==0);
    memset(bytes,'L',page);CHECK(pwrite(fd,bytes,page,page)==page);
    memset(bytes,'H',page);CHECK(pwrite(fd,bytes,page,high)==page);
    memset(bytes,'I',page);CHECK(pwrite(fd,bytes,page,high+page)==page);
    struct stat st;CHECK(fstat(fd,&st)==0&&st.st_size==high+2*page);
    // Only two guest pages are mapped; the file is sparse, not a 4GiB allocation.
    printf("SPARSE_FILE logical=%lld allocated=%lld\n",(long long)st.st_size,(long long)st.st_blocks*512);
    if(argc==2&&!strcmp(argv[1],"--permissions-only")){permissions(high,page);puts("MMAP_PERMISSIONS_PASS");return 0;}
    unsigned char *shared=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,high);
    CHECK(shared!=MAP_FAILED);
    CHECK(shared[0]=='H'&&shared[page]=='I');
    shared[1]='S';CHECK(pread(fd,bytes,2,high)==2&&bytes[0]=='H'&&bytes[1]=='S');
    CHECK(pread(fd,bytes,2,page)==2&&bytes[0]=='L'&&bytes[1]=='L');
    unsigned char *private=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,high);
    CHECK(private!=MAP_FAILED&&private[0]=='H'&&private[page]=='I');
    private[1]='P';CHECK(shared[1]=='S');CHECK(pread(fd,bytes,2,high)==2&&bytes[1]=='S');
    pid_t child=fork();CHECK(child>=0);
    if(!child){shared[2]='C';private[1]='Q';_exit(private[0]=='H'&&private[page]=='I'?0:2);}
    int status=0;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    CHECK(shared[2]=='C'&&private[1]=='P');
    CHECK(munmap(shared,2*page)==0&&munmap(private,2*page)==0);
    unsigned char *low=mmap(NULL,page,PROT_READ,MAP_PRIVATE,fd,page);CHECK(low!=MAP_FAILED&&low[0]=='L'&&low[1]=='L');CHECK(munmap(low,page)==0);
    errno=0;CHECK((void*)syscall(SYS_mmap,NULL,page,PROT_READ,MAP_PRIVATE,fd,(uint64_t)high+1)==MAP_FAILED&&errno==EINVAL);
    errno=0;CHECK((void*)syscall(SYS_mmap,NULL,page,PROT_READ,MAP_PRIVATE,fd,(uint64_t)1<<63)==MAP_FAILED&&errno==EOVERFLOW);
    errno=0;CHECK((void*)syscall(SYS_mmap,NULL,SIZE_MAX,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,(uint64_t)0)==MAP_FAILED&&errno==ENOMEM);
    errno=0;CHECK((void*)syscall(SYS_mmap,NULL,page,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,(uint64_t)1)==MAP_FAILED&&errno==EINVAL);
    permissions(high,page);
    puts("MMAP_OFFSET64_PASS shared private fork COW low_alias host_alignment invalid_offset");printf("checks=%u\n",checks);
    return 0;
}
