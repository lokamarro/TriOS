#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc,char**argv){assert(argc==2);
 if(!strcmp(argv[1],"ordinary")){execl("/bin/cat","cat",(char*)0);return 91;}
 int fd=memfd_create("offload",MFD_ALLOW_SEALING);assert(fd>=0);
 const char*text="prefixNATIVE_MEMFD_STDIN_PASS\n";assert(write(fd,text,strlen(text))==(ssize_t)strlen(text));
 char path[64];snprintf(path,sizeof path,"/proc/self/fd/%d",fd);int ro=open(path,O_RDONLY);assert(ro>=0);close(fd);
 if(!strcmp(argv[1],"stdin")){assert(lseek(ro,6,SEEK_SET)==6&&dup2(ro,0)==0);close(ro);execl("/bin/cat","cat",(char*)0);}
 else if(!strcmp(argv[1],"stdout")){assert(dup2(ro,1)==1);close(ro);execl("/bin/echo","echo","WRITE_MUST_FAIL",(char*)0);}
 return 92;
}
