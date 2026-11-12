#define _GNU_SOURCE
/* Generic named POSIX shared-memory namespace, not a browser-specific test. */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
static void check(int ok, const char *label) {
    if (!ok) { fprintf(stderr,"SHM_NAMESPACE_FAIL %s errno=%d %s\n",label,errno,strerror(errno)); exit(1); }
    printf("SHM_NAMESPACE_PASS %s\n",label);
}
int main(void) {
    setbuf(stdout,NULL); alarm(20);
    struct stat st;
    check(stat("/dev/shm",&st)==0 && S_ISDIR(st.st_mode) && (st.st_mode&01777)==01777 && access("/dev/shm",W_OK|X_OK)==0,"directory sticky/writable/searchable");
    char name[80]; snprintf(name,sizeof name,"/linuxrunner-shm-test-%ld",(long)getpid());
    int rw=shm_open(name,O_CREAT|O_EXCL|O_RDWR,0600);
    check(rw>=0 && (fcntl(rw,F_GETFD)&FD_CLOEXEC),"POSIX create and CLOEXEC");
    int ro=shm_open(name,O_RDONLY,0);
    check(ro>=0 && ftruncate(rw,8192)==0 && shm_unlink(name)==0,"readonly reopen, resize, unlink");
    uint8_t *w=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,rw,0);
    const uint8_t *r=mmap(NULL,8192,PROT_READ,MAP_SHARED,ro,0);
    check(w!=MAP_FAILED && r!=MAP_FAILED,"unlinked descriptor mappings");
    w[4095]=19; w[4096]=23; w[8191]=41;
    check(r[4095]==19 && r[4096]==23 && r[8191]==41,"4 KiB boundary shared coherence");
    pid_t child=fork();
    if(child==0) { w[4096]=73; _exit(r[4096]==73?0:1); }
    int status=0;
    check(child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0 && r[4096]==73,"fork shared mapping coherence");
    check(munmap(w,8192)==0 && munmap((void*)r,8192)==0 && close(rw)==0 && close(ro)==0,"mapping and descriptor cleanup");
    errno=0; int gone=shm_open(name,O_RDONLY,0);
    check(gone<0 && errno==ENOENT,"name remains unlinked");
    puts("SHM_NAMESPACE_ALL_PASS"); return 0;
}
