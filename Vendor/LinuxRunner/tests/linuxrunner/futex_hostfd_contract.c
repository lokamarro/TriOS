#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <string.h>
#include <unistd.h>
#define N 64
static uint32_t word;
static atomic_int entered, finished, failures;
static void *waiter(void *arg) {
    (void)arg;
    struct timespec timeout={.tv_sec=4};
    atomic_fetch_add(&entered,1);
    int rc=syscall(SYS_futex,&word,FUTEX_WAIT_PRIVATE,0,&timeout,0,0);
    if(rc<0 && errno!=EAGAIN) {
        fprintf(stderr,"FUTEX_HOSTFD_ERROR errno=%d\n",errno);
        atomic_fetch_add(&failures,1);
    }
    atomic_fetch_add(&finished,1);return NULL;
}
int main(int argc, char **argv) {
    if(argc>1 && !strcmp(argv[1],"epoll")) {
        int fds[N];
        for(int i=0;i<N;i++) {
            fds[i]=epoll_create1(EPOLL_CLOEXEC);
            if(fds[i]<0) { fprintf(stderr,"EPOLL_HOSTFD_ERROR created=%d errno=%d\n",i,errno); return 1; }
        }
        for(int i=0;i<N;i++)assert(!close(fds[i]));
        puts("EPOLL_HOSTFD_PASS instances=64");return 0;
    }
    alarm(20);
    pthread_t threads[N];
    puts("FUTEX_HOSTFD_BEFORE");fflush(stdout);usleep(1000000);
    for(int i=0;i<N;i++)assert(!pthread_create(&threads[i],0,waiter,0));
    while(atomic_load(&entered)!=N)usleep(1000);
    usleep(200000);
    puts("FUTEX_HOSTFD_DURING");fflush(stdout);usleep(1000000);
    __atomic_store_n(&word,1,__ATOMIC_SEQ_CST);
    assert(syscall(SYS_futex,&word,FUTEX_WAKE_PRIVATE,N,0,0,0)>=0);
    for(int i=0;i<N;i++)assert(!pthread_join(threads[i],0));
    puts("FUTEX_HOSTFD_AFTER");fflush(stdout);usleep(1000000);
    printf("FUTEX_HOSTFD_RESULT threads=%d finished=%d failures=%d\n",N,atomic_load(&finished),atomic_load(&failures));
    return atomic_load(&failures)?1:0;
}
