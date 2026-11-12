#define LR_GPU_GUEST 1
#include "guest_gpu.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/wait.h>
static int create(void){int fd=open("/dev/linuxrunner-gpu",O_RDWR|O_CLOEXEC);assert(fd>=0);struct lr_gpu_request r={.version=1,.operation=LR_GPU_CREATE,.width=32,.height=32};assert(!ioctl(fd,LR_GPU_IOCTL,&r));return fd;}
static void *worker(void *argument){unsigned id=(unsigned)(uintptr_t)argument;int fd=create();unsigned char pixels[32*32*4];
 for(int i=0;i<60;i++){
  struct lr_gpu_request r={.version=1,.operation=LR_GPU_DRAW,.flags=LR_GPU_CLEAR_BIT,.clear={(id+1)/8.f,0,.5f,1}};assert(!ioctl(fd,LR_GPU_IOCTL,&r));
  r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_READ,.width=32,.height=32,.bytes=sizeof pixels,.address=(uintptr_t)pixels};assert(!ioctl(fd,LR_GPU_IOCTL,&r));
  assert(pixels[0]>=(id+1)*32-2&&pixels[0]<=(id+1)*32+2&&pixels[2]>=127&&pixels[2]<=129&&pixels[3]==255);
 }
 struct lr_gpu_request stats={.version=1,.operation=LR_GPU_TRANSFER_INFO};
 assert(!ioctl(fd,LR_GPU_IOCTL,&stats)&&stats.count==1&&stats.bytes==sizeof pixels);
 assert(!close(fd));return NULL;
}
int main(void){
 pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i));for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 int fd=create(),shared=dup(fd);assert(shared>=0);assert(!close(fd));
 pid_t child=fork();assert(child>=0);if(!child){struct lr_gpu_request r={.version=1,.operation=LR_GPU_DRAW,.flags=LR_GPU_CLEAR_BIT,.clear={0,1,0,1}};assert(!ioctl(shared,LR_GPU_IOCTL,&r));assert(!close(shared));_exit(0);}
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 struct lr_gpu_request r={.version=1,.operation=LR_GPU_INFO};assert(!ioctl(shared,LR_GPU_IOCTL,&r)&&r.submissions==1);assert(!close(shared));
 int handles[33];for(int i=0;i<32;i++)handles[i]=create();handles[32]=open("/dev/linuxrunner-gpu",O_RDWR|O_CLOEXEC);assert(handles[32]>=0);
 r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_CREATE,.width=32,.height=32};errno=0;assert(ioctl(handles[32],LR_GPU_IOCTL,&r)==-1&&errno==ENOMEM);
 assert(!close(handles[0]));assert(!ioctl(handles[32],LR_GPU_IOCTL,&r));for(int i=1;i<33;i++)assert(!close(handles[i]));
 puts("GPU_CONCURRENCY_PASS contexts=4 draws=240 readbacks=240 transfer_allocations=4 isolation=PASS dup_fork=PASS quota_recovery=PASS");return 0;
}
