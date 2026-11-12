#define _GNU_SOURCE
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"SYSV_SHM_FAIL line=%d expression=%s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
static void count(int id, unsigned long expected){struct shmid_ds d;CHECK(shmctl(id,IPC_STAT,&d)==0);CHECK(d.shm_nattch==expected);}
static _Atomic int running;
static int race_id;
static void *attach_race(void *unused){(void)unused;while(atomic_load(&running)){void *p=shmat(race_id,0,0);CHECK(p!=(void*)-1);CHECK(shmdt(p)==0);}return NULL;}
int main(int argc,char **argv){
 if(argc==3&&!strcmp(argv[1],"--exec-check")){int id=atoi(argv[2]);count(id,1);puts("SYSV_EXEC_DETACH_PASS");return 0;}
 int id=shmget(IPC_PRIVATE,12289,IPC_CREAT|0600);CHECK(id>=0);
 unsigned char *a=shmat(id,0,0);CHECK(a!=(void*)-1);a[4095]=7;a[4096]=19;a[12288]=31;
 count(id,1);CHECK(shmctl(id,IPC_RMID,0)==0);
 struct shmid_ds d;CHECK(shmctl(id,IPC_STAT,&d)==0);CHECK(d.shm_perm.mode&SHM_DEST);
 unsigned char *b=shmat(id,0,SHM_RDONLY);CHECK(b!=(void*)-1);CHECK(b[4095]==7&&b[4096]==19&&b[12288]==31);count(id,2);CHECK(shmdt(b)==0);
 int sync[2],done[2];CHECK(pipe(sync)==0&&pipe(done)==0);pid_t child=fork();CHECK(child>=0);
 if(!child){close(sync[1]);close(done[0]);char byte;CHECK(read(sync[0],&byte,1)==1);CHECK(a[4096]==19);count(id,2);a[4096]=43;CHECK(shmdt(a)==0);CHECK(write(done[1],"d",1)==1);_exit(0);}
 close(sync[0]);close(done[1]);count(id,2);CHECK(write(sync[1],"s",1)==1);char byte;CHECK(read(done[0],&byte,1)==1);int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);count(id,1);CHECK(a[4096]==43);
 child=fork();CHECK(child>=0);
 if(!child){char text[32];snprintf(text,sizeof text,"%d",id);execl(argv[0],argv[0],"--exec-check",text,(char*)0);_exit(2);}
 CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);count(id,1);
 child=fork();CHECK(child>=0);if(!child)_exit(0);CHECK(waitpid(child,&status,0)==child);count(id,1);
 race_id=id;atomic_store(&running,1);pthread_t thread;CHECK(pthread_create(&thread,0,attach_race,0)==0);
 for(int i=0;i<24;i++){child=fork();CHECK(child>=0);if(!child){CHECK(a[4096]==43);CHECK(shmdt(a)==0);_exit(0);}CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 atomic_store(&running,0);CHECK(pthread_join(thread,0)==0);count(id,1);
 CHECK(shmdt(a)==0);errno=0;CHECK(shmctl(id,IPC_STAT,&d)==-1&&(errno==EINVAL||errno==EIDRM));
 for(int i=0;i<64;i++){id=shmget(IPC_PRIVATE,4096,0600);CHECK(id>=0);a=shmat(id,0,0);CHECK(a!=(void*)-1);CHECK(shmctl(id,IPC_RMID,0)==0);CHECK(shmdt(a)==0);CHECK(shmctl(id,IPC_STAT,&d)==-1);}
 puts("SYSV_SHM_LIFETIME_PASS marked_delete late_attach readonly coherence fork_count child_detach exit_cleanup exec_cleanup repeated_recovery concurrent_attach_fork");return 0;
}
