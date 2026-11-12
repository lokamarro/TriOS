#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static uint32_t words[2];
static atomic_int entered, done;
static int interrupted;
static void *waiter(void *arg) {
    int signal_test=(int)(intptr_t)arg;
    struct timespec timeout={.tv_sec=3};
    atomic_fetch_add(&entered,1);
    int rc=syscall(SYS_futex,&words[0],FUTEX_WAIT_PRIVATE,0,&timeout,0,0);
    if(signal_test) {assert(rc==-1 && errno==EINTR);interrupted=1;}
    else assert(rc==0);
    atomic_fetch_add(&done,1);return 0;
}
static void handler(int sig) {(void)sig;}
static void wait_entered(int n) {while(atomic_load(&entered)<n)usleep(1000);usleep(30000);}
int main(void) {
    alarm(20);
    pthread_t a,b;
    assert(!pthread_create(&a,0,waiter,0));assert(!pthread_create(&b,0,waiter,0));wait_entered(2);
    errno=0;
    assert(syscall(SYS_futex,&words[0],FUTEX_CMP_REQUEUE_PRIVATE,1,(void*)1,&words[1],1)==-1 && errno==EAGAIN);
    assert(syscall(SYS_futex,&words[0],FUTEX_CMP_REQUEUE_PRIVATE,1,(void*)1,&words[1],0)==2);
    assert(syscall(SYS_futex,&words[1],FUTEX_WAKE_PRIVATE,1,0,0,0)==1);
    assert(!pthread_join(a,0));assert(!pthread_join(b,0));assert(atomic_load(&done)==2);
    atomic_store(&entered,0);
    assert(!pthread_create(&a,0,waiter,0));wait_entered(1);
    assert(syscall(SYS_futex,&words[0],FUTEX_WAKE_OP_PRIVATE,1,(void*)1,&words[1],FUTEX_OP(FUTEX_OP_ADD,1,FUTEX_OP_CMP_EQ,0))==1);
    assert(words[1]==1);assert(!pthread_join(a,0));
    struct sigaction action={.sa_handler=handler};sigemptyset(&action.sa_mask);assert(!sigaction(SIGUSR1,&action,0));
    atomic_store(&entered,0);assert(!pthread_create(&a,0,waiter,(void*)1));wait_entered(1);
    assert(!pthread_kill(a,SIGUSR1));assert(!pthread_join(a,0));assert(interrupted);
    uint32_t *shared=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(shared!=MAP_FAILED);
    pid_t child=fork();assert(child>=0);
    if(!child) {struct timespec timeout={.tv_sec=3};int rc=syscall(SYS_futex,shared,FUTEX_WAIT,0,&timeout,0,0);_exit(rc==0?0:1);}
    int woken=0;for(int i=0;i<200&&!woken;i++){woken=syscall(SYS_futex,shared,FUTEX_WAKE,1,0,0,0);assert(woken>=0);if(!woken)usleep(1000);}
    assert(woken==1);int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);assert(!munmap(shared,4096));
    puts("FUTEX_REQUEUE_PASS compare requeue wake_op signal shared_fork");
}
