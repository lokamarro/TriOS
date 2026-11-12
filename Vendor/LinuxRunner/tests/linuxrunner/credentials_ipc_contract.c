#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"CREDENTIAL_IPC_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while(0)
#define ERROR(call,e) do { errno=0; CHECK((call)==-1 && errno==(e)); } while(0)
static void waited(pid_t p) { int s; CHECK(waitpid(p,&s,0)==p && WIFEXITED(s) && WEXITSTATUS(s)==0); }
static void identity(uid_t uid,gid_t gid,gid_t supplementary) {
 CHECK(setgroups(supplementary ? 1:0,supplementary ? &supplementary:NULL)==0);
 CHECK(setgid(gid)==0 && setuid(uid)==0);
}
static void copy_file(const char *src,const char *dst) {
 int in=open(src,O_RDONLY),out=open(dst,O_WRONLY|O_CREAT|O_EXCL,0755);CHECK(in>=0&&out>=0);
 char buf[8192];ssize_t n;while((n=read(in,buf,sizeof(buf)))>0)CHECK(write(out,buf,n)==n);
 CHECK(n==0 && close(in)==0 && close(out)==0);
}
int main(int argc,char **argv) {
 if(argc==2 && !strcmp(argv[1],"--nnp")) {CHECK(getuid()==1001&&geteuid()==1001);CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1);return 0;}
 CHECK(geteuid()==0);
 gid_t old=2001;CHECK(setgroups(1,&old)==0);
 long page=sysconf(_SC_PAGESIZE);char *p=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
 CHECK(mprotect(p+page,page,PROT_NONE)==0);gid_t *bad=(gid_t*)(p+page-sizeof(gid_t));*bad=9999;
 ERROR(syscall(SYS_setgroups,2,bad),EFAULT);gid_t group;CHECK(getgroups(1,&group)==1 && group==old);
 ERROR(syscall(SYS_getgroups,-1,&group),EINVAL);CHECK(munmap(p,page*2)==0);
 ERROR(setuid((uid_t)-1),EINVAL);ERROR(setgid((gid_t)-1),EINVAL);
 char dir[]="/tmp/lr-credentials-XXXXXX",file[256],create[256],suid[256];CHECK(mkdtemp(dir)!=NULL);CHECK(chmod(dir,0755)==0);
 snprintf(file,sizeof file,"%s/protected",dir);snprintf(create,sizeof create,"%s/new",dir);snprintf(suid,sizeof suid,"%s/setid",dir);
 int f=open(file,O_RDWR|O_CREAT|O_EXCL,0640);CHECK(f>=0 && write(f,"original",8)==8 && close(f)==0);CHECK(chown(file,1001,2001)==0);
 pid_t child=fork();CHECK(child>=0);
 if(!child) {identity(1002,2002,2001);f=open(file,O_RDONLY);CHECK(f>=0&&close(f)==0);ERROR(open(file,O_RDWR),EACCES);ERROR(open(file,O_WRONLY|O_TRUNC),EACCES);ERROR(chmod(file,0777),EPERM);ERROR(chown(file,1002,2002),EPERM);ERROR(setgroups(0,NULL),EPERM);ERROR(open(create,O_CREAT|O_WRONLY,0600),EACCES);_exit(0);}waited(child);
 char buf[9]={0};f=open(file,O_RDONLY);CHECK(f>=0&&read(f,buf,8)==8&&!strcmp(buf,"original")&&close(f)==0);
 CHECK(chmod(dir,0555)==0);
 child=fork();CHECK(child>=0);
 if(!child) {identity(1001,2001,0);f=open(file,O_RDONLY|O_CREAT,0600);CHECK(f>=0&&close(f)==0);ERROR(open(create,O_CREAT|O_WRONLY,0600),EACCES);_exit(0);}waited(child);
 // The three-argument ARM64 syscall must ignore a stale flags register.
 CHECK(chown(file,0,0)==0 && chmod(file,0400)==0);
 child=fork();CHECK(child>=0);
 if(!child) {CHECK(setresuid(1001,0,0)==0);ERROR(syscall(SYS_faccessat,-42,file,R_OK,0x200),EACCES);CHECK(syscall(439,-42,file,R_OK,0x200)==0);ERROR(syscall(SYS_faccessat,-42,file,8,0),EINVAL);_exit(0);}waited(child);
 CHECK(chown(file,1001,2001)==0 && chmod(file,0640)==0);
 CHECK(chmod(dir,0777)==0);
 child=fork();CHECK(child>=0);
 if(!child) {identity(1001,2001,0);f=open(create,O_CREAT|O_EXCL|O_WRONLY,0000);CHECK(f>=0&&write(f,"x",1)==1&&close(f)==0);ERROR(open(create,O_RDONLY),EACCES);_exit(0);}waited(child);
 CHECK(unlink(create)==0);
 // Saved-ID updates on setreuid/setregid must follow Linux, not setresuid.
 child=fork();CHECK(child>=0);
 if(!child) {CHECK(setresgid(1001,1002,1003)==0);CHECK(setregid(-1,1003)==0);gid_t a,b,c;CHECK(getresgid(&a,&b,&c)==0&&a==1001&&b==1003&&c==1003);CHECK(setresuid(1001,1002,1003)==0);CHECK(setreuid(-1,1003)==0);uid_t x,y,z;CHECK(getresuid(&x,&y,&z)==0&&x==1001&&y==1003&&z==1003);ERROR(setreuid(1002,-1),EPERM);_exit(0);}waited(child);
 copy_file(argv[0],suid);CHECK(chmod(suid,04755)==0);
 child=fork();CHECK(child>=0);
 if(!child) {identity(1001,2001,0);ERROR(prctl(PR_SET_NO_NEW_PRIVS,0,0,0,0),EINVAL);CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0);ERROR(prctl(PR_SET_NO_NEW_PRIVS,0,0,0,0),EINVAL);execl(suid,suid,"--nnp",(char*)0);CHECK(0);}waited(child);
 int id=shmget(0x5a000000 | (getpid()&0xffff),4097,IPC_CREAT|IPC_EXCL|0640);CHECK(id>=0);struct shmid_ds d;CHECK(shmctl(id,IPC_STAT,&d)==0);
 CHECK(d.shm_perm.uid==0&&d.shm_perm.cuid==0&&d.shm_perm.mode==0640&&d.shm_segsz==4097&&d.shm_cpid==getpid()&&d.shm_ctime>0);
 d.shm_perm.uid=1001;d.shm_perm.gid=2001;CHECK(shmctl(id,IPC_SET,&d)==0);
 CHECK(shmctl(id,IPC_STAT,&d)==0&&d.shm_perm.uid==1001&&d.shm_perm.cuid==0&&d.shm_perm.gid==2001);
 ERROR(shmctl(id,IPC_SET,(void*)1),EFAULT);d.shm_perm.uid=(uid_t)-1;ERROR(shmctl(id,IPC_SET,&d),EINVAL);
 child=fork();CHECK(child>=0);
 if(!child) {identity(1002,2002,2001);CHECK(shmget(0x5a000000 | (getppid()&0xffff),0,0400)==id);ERROR(shmget(0x5a000000 | (getppid()&0xffff),0,0600),EACCES);CHECK(shmctl(id,IPC_STAT,&d)==0);void *a=shmat(id,NULL,SHM_RDONLY);CHECK(a!=(void*)-1&&shmdt(a)==0);ERROR(shmat(id,NULL,0),EACCES);ERROR(shmctl(id,IPC_RMID,NULL),EPERM);ERROR(shmctl(id,IPC_SET,&d),EPERM);_exit(0);}waited(child);
 child=fork();CHECK(child>=0);
 if(!child) {identity(1003,2003,0);ERROR(shmctl(id,IPC_STAT,&d),EACCES);ERROR(shmat(id,NULL,SHM_RDONLY),EACCES);_exit(0);}waited(child);
 child=fork();CHECK(child>=0);
 if(!child) {identity(1001,2001,0);void *a=shmat(id,NULL,0);CHECK(a!=(void*)-1);CHECK(shmctl(id,IPC_RMID,NULL)==0);CHECK(shmctl(id,IPC_STAT,&d)==0&&(d.shm_perm.mode&SHM_DEST)&&d.shm_atime>0&&d.shm_lpid==getpid());CHECK(shmdt(a)==0);_exit(0);}waited(child);
 ERROR(shmctl(id,IPC_STAT,&d),EINVAL);
 CHECK(unlink(suid)==0&&unlink(file)==0&&rmdir(dir)==0);
 printf("CREDENTIAL_IPC_CONTRACT_PASS checks=%u groups_dac_no_truncate_saved_ids_no_new_privs_shm_permissions\n",checks);return 0;
}
