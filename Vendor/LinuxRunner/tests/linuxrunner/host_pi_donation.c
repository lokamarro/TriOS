// Test the exact guest dependency-propagation implementation against public
// Darwin QoS overrides. No guest workload, Linux RT, or device claim is made.
#include <mach/mach.h>
#include <pthread/qos.h>
#include <unistd.h>
#include <assert.h>
#include <stdarg.h>
#include "kernel/futex.c"
void ish_printk(const char *fmt, ...) { va_list args; va_start(args,fmt); vfprintf(stderr,fmt,args); va_end(args); }
static struct task owners[3];
lock_t pids_lock = LOCK_INITIALIZER;
struct task *pid_get_task(dword_t tid) {return tid >= 1 && tid <= 2 ? &owners[tid] : NULL;}
static atomic_int ready, stop;
static void *worker(void *p){(void)p;assert(!pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND,0));atomic_fetch_add(&ready,1);while(!stop)usleep(1000);return NULL;}
static int priority(pthread_t t){thread_extended_info_data_t info;mach_msg_type_number_t count=THREAD_EXTENDED_INFO_COUNT;assert(thread_info(pthread_mach_thread_np(t),THREAD_EXTENDED_INFO,(thread_info_t)&info,&count)==KERN_SUCCESS);return info.pth_priority;}
int main(void){
 pthread_mutexattr_t attr; pthread_mutex_t mutex;
 assert(!pthread_mutexattr_init(&attr)); int protocol=pthread_mutexattr_setprotocol(&attr,PTHREAD_PRIO_INHERIT);
 int initialized=protocol ? protocol : pthread_mutex_init(&mutex,&attr);
 printf("HOST_PI_API: protocol_request=%d mutex_init=%d (acceptance alone is not a donation test)\n",protocol,initialized);
 if(!initialized)assert(!pthread_mutex_destroy(&mutex));assert(!pthread_mutexattr_destroy(&attr));
 int policy;struct sched_param original;assert(!pthread_getschedparam(pthread_self(),&policy,&original));
 struct sched_param rt={.sched_priority=sched_get_priority_min(SCHED_FIFO)};
 int requested=rt.sched_priority<0 ? errno : pthread_setschedparam(pthread_self(),SCHED_FIFO,&rt);
 int observed;struct sched_param resulting;assert(!pthread_getschedparam(pthread_self(),&observed,&resulting));
 printf("HOST_SCHED_API: FIFO_request=%d observed_policy=%d priority=%d (host only; no guest RT claim)\n",requested,observed,resulting.sched_priority);
 if(!requested)assert(!pthread_setschedparam(pthread_self(),policy,&original));
 for(unsigned i=1;i<=2;i++){owners[i].pid=i;assert(!pthread_create(&owners[i].thread,NULL,worker,NULL));}
 while(ready!=2)usleep(1000);
 struct task donor={.pid=3};
 struct pi_state a={.owner=1},b={.owner=2};list_init(&a.queue);list_init(&b.queue);list_add_tail(&pi_states,&a.link);list_add_tail(&pi_states,&b.link);pi_state_count=2;
 struct pi_waiter wa={.task=&owners[2],.qos=QOS_CLASS_UTILITY},wb={.task=&donor,.qos=QOS_CLASS_USER_INITIATED};list_add_tail(&a.queue,&wa.link);list_add_tail(&b.queue,&wb.link);
 int before1=priority(owners[1].thread),before2=priority(owners[2].thread);
 lock(&pi_lock);pi_update_donations();unlock(&pi_lock);usleep(20000);
 int during1=priority(owners[1].thread),during2=priority(owners[2].thread);
 assert(a.qos==QOS_CLASS_USER_INITIATED&&b.qos==QOS_CLASS_USER_INITIATED);assert(a.donation&&b.donation);assert(during1>before1&&during2>before2);
 lock(&pi_lock);list_remove(&wb.link);pi_update_donations();unlock(&pi_lock);usleep(20000);
 int lowered1=priority(owners[1].thread),restored2=priority(owners[2].thread);assert(lowered1<during1&&lowered1>before1);assert(restored2==before2);
 lock(&pi_lock);list_remove(&wa.link);pi_update_donations();unlock(&pi_lock);usleep(20000);
 int restored1=priority(owners[1].thread);assert(restored1==before1);
 printf("HOST_PI_DONATION_PASS chain priorities: A=%d->%d->%d->%d B=%d->%d->%d starts=%llu failures=%llu\n",before1,during1,lowered1,restored1,before2,during2,restored2,(unsigned long long)pi_donation_starts,(unsigned long long)pi_donation_failures);
 stop=1;for(unsigned i=1;i<=2;i++)assert(!pthread_join(owners[i].thread,NULL));return 0;
}
