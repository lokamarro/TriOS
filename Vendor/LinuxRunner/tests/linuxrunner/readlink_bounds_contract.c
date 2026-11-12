#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void){char root[]="/tmp/lr-readlink-XXXXXX";assert(mkdtemp(root));char file[160],link[160],buf[64];snprintf(file,sizeof file,"%s/file",root);snprintf(link,sizeof link,"%s/link",root);int f=open(file,O_CREAT|O_WRONLY,0600);assert(f>=0);close(f);assert(!symlink("file",link));errno=0;assert(readlink(file,buf,sizeof buf)==-1&&errno==EINVAL);errno=0;assert(syscall(SYS_readlinkat,AT_FDCWD,link,buf,0)==-1&&errno==EINVAL);errno=0;assert(syscall(SYS_readlinkat,AT_FDCWD,link,buf,0x80000000U)==-1&&errno==EINVAL);memset(buf,0xaa,sizeof buf);assert(readlink(link,buf,2)==2&&!memcmp(buf,"fi",2)&&(unsigned char)buf[2]==0xaa);assert(syscall(SYS_readlinkat,AT_FDCWD,link,buf,0x7fffffff)==4&&!memcmp(buf,"file",4));unlink(link);unlink(file);rmdir(root);puts("READLINK_BOUNDS PASS regular EINVAL, zero/signed overflow, bounded huge size and no-NUL truncation");return 0;}
