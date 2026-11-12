#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
int main(void){char path[80];snprintf(path,sizeof path,"/tmp/dirent-types-%d",getpid());if(mkdir(path,0700)||chdir(path))return 2;int fd=open("file",O_CREAT|O_WRONLY,0600);if(fd<0)return 2;close(fd);if(symlink("file","link")||mkdir("directory",0700))return 2;DIR *d=opendir(".");struct dirent *e;int failures=0,seen=0;while((e=readdir(d))){if(e->d_name[0]=='.')continue;struct stat s;if(lstat(e->d_name,&s))return 2;unsigned expected=(s.st_mode>>12)&15;seen++;if(e->d_type!=expected){printf("FAIL: %s d_type=%u expected=%u\n",e->d_name,e->d_type,expected);failures++;}}closedir(d);unlink("link");unlink("file");rmdir("directory");chdir("/");rmdir(path);if(seen!=3)return 2;puts(failures?"FAIL: guest dirent types":"PASS: guest dirent types agree with lstat");return failures?1:0;}
