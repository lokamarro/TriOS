#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t child_pid,child_code,child_status;
static void on_child(int sig,siginfo_t *info,void *context){(void)sig;(void)context;child_pid=info->si_pid;child_code=info->si_code;child_status=info->si_status;}
static void *worker(void *unused){(void)unused;usleep(50000);syscall(SYS_exit_group,191);return 0;}
int main(void){struct sigaction a={.sa_sigaction=on_child,.sa_flags=SA_SIGINFO};sigemptyset(&a.sa_mask);assert(!sigaction(SIGCHLD,&a,0));pid_t pid=fork();assert(pid>=0);if(!pid){pthread_t t;assert(!pthread_create(&t,0,worker,0));for(;;)pause();}int status,r;do{r=waitpid(pid,&status,0);}while(r<0&&errno==EINTR);assert(r==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==191);for(int i=0;i<100&&!child_pid;i++)usleep(1000);printf("GROUP_EXIT child=%d sigchld_pid=%d code=%d status=%d wait_status=%d\n",pid,child_pid,child_code,child_status,WEXITSTATUS(status));assert(child_pid==pid&&child_code==CLD_EXITED&&child_status==191);puts("THREAD_GROUP_EXIT PASS leader identity, SIGCHLD decoded status and worker exit_group191");return 0;}
