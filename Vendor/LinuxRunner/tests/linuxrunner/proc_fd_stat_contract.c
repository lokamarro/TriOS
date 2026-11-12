#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"PROC_FD_STAT_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void verify(int fd){
 struct stat held,st;char path[80],name[32];CHECK(fstat(fd,&held)==0);
 snprintf(path,sizeof path,"/proc/self/fd/%d",fd);CHECK(stat(path,&st)==0&&st.st_ino==held.st_ino&&st.st_mode==held.st_mode&&st.st_size==held.st_size);
 CHECK(lstat(path,&st)==0&&S_ISLNK(st.st_mode));
 int d=open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(d>=0);snprintf(name,sizeof name,"%d",fd);
 CHECK(fstatat(d,name,&st,0)==0&&st.st_ino==held.st_ino&&st.st_size==held.st_size);close(d);
 snprintf(path,sizeof path,"/dev/fd/%d",fd);CHECK(stat(path,&st)==0&&st.st_ino==held.st_ino);
}
static void run(void){
 char path[]="/tmp/lr-proc-stat-XXXXXX";int f=mkstemp(path);CHECK(f>=0&&write(f,"abc",3)==3);verify(f);CHECK(unlink(path)==0);verify(f);
 int m=memfd_create("proc-fd-test",MFD_CLOEXEC);CHECK(m>=0&&ftruncate(m,4097)==0);verify(m);
 int p[2],s[2];CHECK(pipe(p)==0&&socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);verify(p[0]);verify(s[0]);
 // Chromium's HasOpenDirectory requires fstatat to succeed for every live fd.
 DIR *d=opendir("/proc/self/fd");CHECK(d!=NULL);struct dirent *e;
 while((e=readdir(d))){if(e->d_name[0]=='.')continue;struct stat st;CHECK(fstatat(dirfd(d),e->d_name,&st,0)==0);}
 closedir(d);close(f);close(m);close(p[0]);close(p[1]);close(s[0]);close(s[1]);printf("PROC_FD_STAT_PASS uid=%u checks=%u named unlinked memfd pipe socket enumeration\n",getuid(),checks);
}

int main(void){run();fflush(stdout);pid_t p=fork();CHECK(p>=0);if(!p){CHECK(setgid(1001)==0&&setuid(1001)==0);checks=0;run();fflush(stdout);_exit(0);}int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&WEXITSTATUS(status)==0);return 0;}
