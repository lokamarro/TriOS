#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void){
 char p[]="/tmp/lr-access-XXXXXX";int fd=mkstemp(p);if(fd<0||fchmod(fd,0400)||setresuid(1001,0,0))return 2;
 errno=0;long legacy=syscall(SYS_faccessat,-42,p,R_OK,0x200);int e=errno;
 long effective=syscall(439,-42,p,R_OK,0x200);int ok=legacy==-1&&e==EACCES&&effective==0;
 close(fd);unlink(p);if(!ok){fprintf(stderr,"FACCESSAT_ABI_FAIL legacy=%ld errno=%d effective=%ld\n",legacy,e,effective);return 1;}
 puts("FACCESSAT_ABI_PASS legacy_ignores_x3=PASS real_uid=PASS effective_uid=PASS absolute_dirfd=PASS");return 0;
}
