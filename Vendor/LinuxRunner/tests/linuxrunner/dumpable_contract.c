#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"DUMPABLE_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static void wait_ok(pid_t p){int s;CHECK(waitpid(p,&s,0)==p&&WIFEXITED(s)&&WEXITSTATUS(s)==0);}
static void own_fds(void) {
 char path[128];struct stat st;
 const char *forms[]={"/proc/self/fd","/proc/thread-self/fd",path};
 snprintf(path,sizeof path,"/proc/%d/task/%ld/fd",getpid(),syscall(SYS_gettid));
 for(unsigned i=0;i<3;i++) {
  CHECK(stat(forms[i],&st)==0&&st.st_uid==0);
  DIR *d=opendir(forms[i]);CHECK(d!=NULL);unsigned found=0;struct dirent *entry;
  while((entry=readdir(d)))if(!strcmp(entry->d_name,"1"))found++;
  CHECK(found==1);int f=openat(dirfd(d),".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(f>=0);close(f);CHECK(closedir(d)==0);
 }
}
static void *own_fd_thread(void *unused){(void)unused;own_fds();return NULL;}
static void own_inspection(void) {
 pid_t p=fork();CHECK(p>=0);if(!p){
  CHECK(setgid(1001)==0&&setuid(1001)==0);CHECK(prctl(PR_GET_DUMPABLE)==0);own_fds();
  pthread_t thread;CHECK(pthread_create(&thread,NULL,own_fd_thread,NULL)==0&&pthread_join(thread,NULL)==0);_exit(0);
 }wait_ok(p);
}
static void *thread_policy(void *unused){(void)unused;CHECK(prctl(PR_GET_DUMPABLE)==0);CHECK(prctl(PR_SET_DUMPABLE,1L)==0);return NULL;}
static const char secret[]="guest-mm-inspection";
static void inspect(pid_t target,int allowed){
 char path[128],buf[128];struct stat st;
 const char *files[]={"maps","environ","auxv","mem"};
 for(unsigned i=0;i<4;i++){
  snprintf(path,sizeof path,"/proc/%d/%s",target,files[i]);
  errno=0;int f=open(path,O_RDONLY);ssize_t n=f<0?-1:i==3?pread(f,buf,sizeof secret,(off_t)(unsigned long)secret):read(f,buf,sizeof buf);
  int error=errno;if(f>=0)close(f);
  CHECK(allowed?n>0:n==-1&&(error==EACCES||error==EPERM));
 }
 snprintf(path,sizeof path,"/proc/%d/exe",target);errno=0;ssize_t n=readlink(path,buf,sizeof buf);CHECK(allowed?n>0:n==-1&&errno==EACCES);
 snprintf(path,sizeof path,"/proc/%d/fd/1",target);errno=0;n=readlink(path,buf,sizeof buf);CHECK(allowed?n>0:n==-1&&errno==EACCES);
 snprintf(path,sizeof path,"/proc/%d/fd",target);errno=0;int f=open(path,O_RDONLY|O_DIRECTORY);int error=errno;if(f>=0)close(f);CHECK(allowed?f>=0:f==-1&&error==EACCES);
 snprintf(path,sizeof path,"/proc/%d/environ",target);CHECK(stat(path,&st)==0&&st.st_uid==(allowed?1001:0));
}
static void foreign_inspection(void){
 int command[2],ack[2];CHECK(pipe(command)==0&&pipe(ack)==0);pid_t target=fork();CHECK(target>=0);
 if(!target){close(command[1]);close(ack[0]);CHECK(setgid(1001)==0&&setuid(1001)==0);CHECK(prctl(PR_GET_DUMPABLE)==0);CHECK(prctl(PR_SET_DUMPABLE,1L)==0);char state='1';CHECK(write(ack[1],&state,1)==1);while(read(command[0],&state,1)==1&&state!='x'){CHECK(prctl(PR_SET_DUMPABLE,(long)(state=='1'))==0);CHECK(write(ack[1],&state,1)==1);}_exit(0);}
 close(command[0]);close(ack[1]);char state;CHECK(read(ack[0],&state,1)==1);
 for(unsigned phase=0;phase<3;phase++){
  int allowed=phase!=1;
  if(phase){state=allowed?'1':'0';CHECK(write(command[1],&state,1)==1&&read(ack[0],&state,1)==1);}
  pid_t reader=fork();CHECK(reader>=0);if(!reader){CHECK(setgid(1001)==0&&setuid(1001)==0);inspect(target,allowed);_exit(0);}wait_ok(reader);
 }
 state='x';CHECK(write(command[1],&state,1)==1);close(command[1]);close(ack[0]);wait_ok(target);
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--exec")){CHECK(prctl(PR_GET_DUMPABLE)==1);return 0;}
 if(argc==2&&!strcmp(argv[1],"--setid")){CHECK(getuid()==1001&&geteuid()==0&&prctl(PR_GET_DUMPABLE)==0);return 0;}
 CHECK(prctl(PR_GET_DUMPABLE)==1);errno=0;CHECK(prctl(PR_SET_DUMPABLE,2L)==-1&&errno==EINVAL);
 CHECK(prctl(PR_SET_DUMPABLE,0L)==0&&prctl(PR_GET_DUMPABLE)==0);
 pthread_t thread;CHECK(pthread_create(&thread,NULL,thread_policy,NULL)==0&&pthread_join(thread,NULL)==0);CHECK(prctl(PR_GET_DUMPABLE)==1);
 CHECK(prctl(PR_SET_DUMPABLE,0L)==0);pid_t p=fork();CHECK(p>=0);if(!p){CHECK(prctl(PR_GET_DUMPABLE)==0);CHECK(prctl(PR_SET_DUMPABLE,1L)==0);_exit(0);}wait_ok(p);CHECK(prctl(PR_GET_DUMPABLE)==0);
 p=fork();CHECK(p>=0);if(!p){execl(argv[0],argv[0],"--exec",NULL);_exit(90);}wait_ok(p);
 char *args[]={"missing",NULL};errno=0;CHECK(execv("/definitely-not-a-linuxrunner-file",args)==-1&&errno==ENOENT);CHECK(prctl(PR_GET_DUMPABLE)==0);
 CHECK(prctl(PR_SET_DUMPABLE,1L)==0);
 own_inspection();
 foreign_inspection();
 // Effective credential changes reset the shared mm policy, even if the
 // saved identity permits returning to root later.
 p=fork();CHECK(p>=0);if(!p){CHECK(setresuid(-1,1001,-1)==0);CHECK(prctl(PR_GET_DUMPABLE)==0);CHECK(prctl(PR_SET_DUMPABLE,1L)==0);CHECK(setresuid(-1,0,-1)==0);CHECK(prctl(PR_GET_DUMPABLE)==0);_exit(0);}wait_ok(p);
 p=fork();CHECK(p>=0);if(!p){CHECK(setresgid(-1,1001,-1)==0);CHECK(prctl(PR_GET_DUMPABLE)==0);_exit(0);}wait_ok(p);
 char path[]="/tmp/lr-dumpable-exec-XXXXXX",data[8192];int dest=mkstemp(path),src=open(argv[0],O_RDONLY);CHECK(dest>=0&&src>=0);ssize_t n;
 while((n=read(src,data,sizeof data))>0)CHECK(write(dest,data,n)==n);
 CHECK(n==0&&close(src)==0&&close(dest)==0&&chmod(path,04755)==0);
 p=fork();CHECK(p>=0);if(!p){CHECK(setuid(1001)==0);execl(path,path,"--setid",NULL);_exit(91);}wait_ok(p);CHECK(unlink(path)==0);
 printf("DUMPABLE_CONTRACT_PASS parent_checks=%u thread_fork_exec_credentials_proc=PASS\n",checks);return 0;
}
