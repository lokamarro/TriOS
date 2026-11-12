#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef FUTEX_LOCK_PI2
#define FUTEX_LOCK_PI2 13
#endif
static _Atomic unsigned word, stage;
static long fut(_Atomic unsigned *w,int op,struct timespec *t){return syscall(SYS_futex,w,op,0,t,0,0);}
static unsigned tid(void){return syscall(SYS_gettid);}
static void ms(int n){struct timespec t={n/1000,n%1000*1000000};nanosleep(&t,0);}
static struct timespec deadline(clockid_t c,int n){struct timespec t;clock_gettime(c,&t);t.tv_sec+=n/1000;t.tv_nsec+=n%1000*1000000;if(t.tv_nsec>=1000000000){t.tv_sec++;t.tv_nsec-=1000000000;}return t;}
static void *alias_wait(void*p){_Atomic unsigned*w=p;struct timespec t=deadline(CLOCK_REALTIME,1500);stage=1;int r=fut(w,FUTEX_LOCK_PI,&t);if(r){perror("PI shared alias lock");return (void*)1;}assert((*w&FUTEX_TID_MASK)==tid());assert(!fut(w,FUTEX_UNLOCK_PI,0));return 0;}
static void *dying(void*p){(void)p;assert(!fut(&word,FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,0));stage=1;while(!(word&FUTEX_WAITERS))ms(1);return 0;}
static void *recover(void*p){(void)p;assert(!fut(&word,FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,0));assert((word&FUTEX_TID_MASK)==tid());assert(word&FUTEX_OWNER_DIED);assert(!fut(&word,FUTEX_UNLOCK_PI|FUTEX_PRIVATE_FLAG,0));return 0;}
static void handler(int s){(void)s;}
static void *ordinary(void*p){(void)p;stage=1;errno=0;assert(syscall(SYS_futex,&word,FUTEX_WAIT|FUTEX_PRIVATE_FLAG,word,0,0,0)==-1&&errno==EINTR);return 0;}
static void *try_mixed(void*p){(void)p;errno=0;assert(fut(&word,FUTEX_TRYLOCK_PI|FUTEX_PRIVATE_FLAG,0)==-1&&errno==EINVAL);return 0;}
static void *pi_wait(void*p){(void)p;struct timespec t=deadline(CLOCK_MONOTONIC,100);errno=0;assert(fut(&word,FUTEX_LOCK_PI2|FUTEX_PRIVATE_FLAG,&t)==-1&&errno==ETIMEDOUT);return 0;}
int main(void){alarm(30);pthread_t t,u;void *result;
 int fd=syscall(SYS_memfd_create,"pi-alias",0);assert(fd>=0);assert(!ftruncate(fd,16384));_Atomic unsigned *a=mmap(0,16384,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0),*b=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,4096);assert(a!=MAP_FAILED&&b!=MAP_FAILED);_Atomic unsigned *first=(_Atomic unsigned*)((char*)a+4096);assert(!fut(first,FUTEX_LOCK_PI,0));assert(!pthread_create(&t,0,alias_wait,b));while(!stage)ms(1);ms(50);assert(!fut(first,FUTEX_UNLOCK_PI,0));assert(!pthread_join(t,&result));assert(!result);assert(!*first);munmap(a,16384);munmap(b,4096);close(fd);puts("PI alias 4K-offset/16K-host PASS");
 stage=0;assert(!pthread_create(&t,0,dying,0));while(!stage)ms(1);assert(!pthread_create(&u,0,recover,0));assert(!pthread_join(t,0));assert(!pthread_join(u,0));assert(!word);puts("PI contended nonrobust owner termination PASS");
 struct sigaction sa={.sa_handler=handler};sigemptyset(&sa.sa_mask);sigaction(SIGUSR1,&sa,0);assert(!fut(&word,FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,0));stage=0;assert(!pthread_create(&t,0,ordinary,0));while(!stage)ms(1);ms(50);assert(!pthread_create(&u,0,try_mixed,0));assert(!pthread_join(u,0));assert(!pthread_kill(t,SIGUSR1));assert(!pthread_join(t,0));assert(!fut(&word,FUTEX_UNLOCK_PI|FUTEX_PRIVATE_FLAG,0));puts("PI/non-PI mixing rejected PASS");
 assert(!fut(&word,FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,0));assert(!pthread_create(&t,0,pi_wait,0));for(int i=0;!(word&FUTEX_WAITERS)&&i<1000;i++)ms(1);errno=0;assert(syscall(SYS_futex,&word,FUTEX_WAKE|FUTEX_PRIVATE_FLAG,1,0,0,0)==-1&&errno==EINVAL);assert(!pthread_join(t,0));assert(!fut(&word,FUTEX_UNLOCK_PI|FUTEX_PRIVATE_FLAG,0));puts("PI2 absolute monotonic timeout/non-PI wake PASS");
 word=FUTEX_TID_MASK;errno=0;assert(fut(&word,FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,0)==-1&&errno==ESRCH);assert(word&FUTEX_WAITERS);word=0;errno=0;assert(fut(&word,FUTEX_LOCK_PI|FUTEX_CLOCK_REALTIME,0)==-1&&errno==ENOSYS);errno=0;assert(fut((_Atomic unsigned*)((char*)&word+1),FUTEX_LOCK_PI,0)==-1&&errno==EINVAL);errno=0;assert(fut((_Atomic unsigned*)0,FUTEX_LOCK_PI,0)==-1&&errno==EFAULT);puts("PI bad owner/clock/alignment/address PASS");
 puts("FUTEX_PI_EDGES_PASS");}
