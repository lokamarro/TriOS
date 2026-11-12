#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc,char **argv){
 if(argc>1&&!strcmp(argv[1],"--child")){assert(argc==34);for(int i=2;i<argc;i++){assert(strlen(argv[i])==1023);for(int j=0;j<1023;j++)assert(argv[i][j]=='A'+i%26);}const char *e=getenv("LR_COPY_TEST");assert(e&&strlen(e)==1023&&e[1022]=='e');return 0;}
 if(argc>1&&!strcmp(argv[1],"--guard")){assert(argc==3&&!argv[2][0]);return 0;}
 char text[32][1024],env[1037];char *args[35]={argv[0],"--child"},*environment[]={env,NULL};memcpy(env,"LR_COPY_TEST=",13);memset(env+13,'e',1023);env[1036]=0;
 for(int i=0;i<32;i++){memset(text[i],'A'+(i+2)%26,1023);text[i][1023]=0;args[i+2]=text[i];}
 for(int i=0;i<20;i++){pid_t child=fork();assert(child>=0);if(!child){execve(argv[0],args,environment);_exit(127);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 char *p=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED&&!mprotect(p+4096,4096,PROT_NONE));p[4095]=0;
 char *guard[]={argv[0],"--guard",p+4095,NULL};pid_t child=fork();assert(child>=0);if(!child){execve(argv[0],guard,environment);_exit(127);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 p[4095]='x';errno=0;assert(execve(argv[0],guard,environment)==-1&&errno==EFAULT);assert(!munmap(p,8192));puts("EXEC_ARGUMENT_COPY_PASS repeated_exec=20 large_argv_environment=PASS page_guard_terminator=PASS EFAULT=PASS");
}
