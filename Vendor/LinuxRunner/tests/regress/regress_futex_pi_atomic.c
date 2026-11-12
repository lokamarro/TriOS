#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
// Atomic contract remains independently tested alongside the contention suite.
static int word;static long owner;
static long op(int cmd){return syscall(SYS_futex,&word,cmd|FUTEX_PRIVATE_FLAG,0,0,0,0);}
static void *other(void *p){(void)p;errno=0;assert(op(FUTEX_UNLOCK_PI)==-1&&errno==EPERM);assert(word==owner);errno=0;assert(op(FUTEX_TRYLOCK_PI)==-1&&errno==EAGAIN);assert((word&FUTEX_TID_MASK)==owner && (word&FUTEX_WAITERS));return 0;}
int main(void){alarm(20);owner=syscall(SYS_gettid);
 for(int i=0;i<2000;i++){assert(op(FUTEX_LOCK_PI)==0&&word==owner);assert(op(FUTEX_UNLOCK_PI)==0&&!word);}
 assert(op(FUTEX_TRYLOCK_PI)==0&&word==owner);errno=0;assert(op(FUTEX_LOCK_PI)==-1&&errno==EDEADLK);
 pthread_t t;assert(!pthread_create(&t,0,other,0));assert(!pthread_join(t,0));assert(op(FUTEX_UNLOCK_PI)==0);
 word=FUTEX_OWNER_DIED;assert(op(FUTEX_LOCK_PI)==0&&word==(owner|FUTEX_OWNER_DIED));assert(op(FUTEX_UNLOCK_PI)==0&&!word);
 struct timespec bad={0,1000000000};errno=0;assert(syscall(SYS_futex,&word,FUTEX_LOCK_PI|FUTEX_PRIVATE_FLAG,0,&bad,0,0)==-1&&errno==EINVAL);assert(!word);
 puts("FUTEX_PI_ATOMIC_PASS lock trylock unlock self_deadlock wrong_owner cycles owner_died");}
