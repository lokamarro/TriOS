#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int failed,checks;
#define CHECK(name,x) do { int good=!!(x); checks++;failed+=!good;printf("FD_UNSHARE %s %s\n",name,good?"PASS":"FAIL");fflush(stdout); }while(0)
struct args{int fd,flags;const char *self,*invalid;};
static void *range_worker(void *opaque){struct args*a=opaque;int r=syscall(SYS_close_range,a->fd,a->fd,a->flags);
 int flags=fcntl(a->fd,F_GETFD);return (void*)(uintptr_t)(r!=0 || (a->flags&4 ? flags!=FD_CLOEXEC : !(flags<0&&errno==EBADF)));}
static int execute_child(void *opaque){struct args*a=opaque;char number[20];snprintf(number,sizeof number,"%d",a->fd);
 execl(a->self,a->self,"--closed",number,NULL);return 8;}
static int failed_child(void *opaque){struct args*a=opaque;execl(a->invalid,a->invalid,NULL);if(errno!=ENOEXEC)return 9;return close(a->fd)!=0;}
static int cloned(int(*fn)(void*),struct args *args){char *stack=malloc(131072);if(!stack)return 10;pid_t p=clone(fn,stack+131072,CLONE_FILES|SIGCHLD,args);int status=0;
 if(p<0||waitpid(p,&status,0)!=p){free(stack);return 11;}free(stack);return WIFEXITED(status)?WEXITSTATUS(status):12;}
int main(int argc,char **argv){if(argc==3&&!strcmp(argv[1],"--closed")){int r=fcntl(atoi(argv[2]),F_GETFD);return !(r<0&&errno==EBADF);}
 for(int mode=0;mode<2;mode++){int fd=open("/dev/null",O_RDONLY);struct args a={.fd=fd,.flags=2|(mode?4:0)};pthread_t t;void *status;
 CHECK("range_thread_start",fd>=0&&!pthread_create(&t,NULL,range_worker,&a));pthread_join(t,&status);CHECK("range_worker_semantics",status==NULL);
 CHECK(mode?"cloexec_table_isolation":"close_table_isolation",fcntl(fd,F_GETFD)==0);close(fd);}
 int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);struct args a={.fd=fd,.self=argv[0]};CHECK("clone_exec_child",cloned(execute_child,&a)==0);
 CHECK("exec_parent_descriptor",fcntl(fd,F_GETFD)==FD_CLOEXEC);close(fd);
 char invalid[]="/tmp/lr-invalid-exec.XXXXXX";int invalid_fd=mkstemp(invalid);write(invalid_fd,"invalid",7);fchmod(invalid_fd,0755);close(invalid_fd);
 fd=open("/dev/null",O_RDONLY);a=(struct args){.fd=fd,.invalid=invalid};CHECK("failed_exec_child",cloned(failed_child,&a)==0);
 CHECK("failed_exec_preserves_sharing",fcntl(fd,F_GETFD)<0&&errno==EBADF);unlink(invalid);
 printf("FD_UNSHARE_CONTRACT checks=%d failures=%d\n",checks,failed);return !!failed;}
