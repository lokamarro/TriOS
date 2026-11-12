#define LR_GPU_GUEST 1
#include "guest_gpu.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/mman.h>
static void failure(int fd,struct lr_gpu_request *r,int e){errno=0;assert(ioctl(fd,LR_GPU_IOCTL,r)==-1&&errno==e);}
int main(void){
 int fd=open("/dev/linuxrunner-gpu",O_RDWR|O_CLOEXEC);assert(fd>=0);
 errno=0;assert(ioctl(fd,0x1234,NULL)==-1&&errno==ENOTTY);
 struct lr_gpu_request r={.version=99,.operation=LR_GPU_CREATE,.width=32,.height=32};failure(fd,&r,EINVAL);
 r.version=1;r.width=LR_GPU_MAX_DIMENSION+1;failure(fd,&r,EINVAL);r.width=32;r.flags=1;failure(fd,&r,EINVAL);r.flags=0;
 assert(ioctl(fd,LR_GPU_IOCTL,&r)==0);failure(fd,&r,EBUSY);
 r.operation=999;failure(fd,&r,EOPNOTSUPP);
 r.operation=LR_GPU_READ;r.bytes=32*32*4;r.address=1;failure(fd,&r,EINVAL); // no rendered target yet
 r.operation=LR_GPU_DRAW;r.flags=LR_GPU_TRIANGLES_BIT;r.count=2;r.bytes=64;failure(fd,&r,EINVAL);
 r.count=3;r.bytes=96;r.address=1;failure(fd,&r,EFAULT);
 struct lr_gpu_vertex vertices[3]={{{0,0,0,1},{1,0,0,1}},{{.5,0,0,1},{1,0,0,1}},{{0,.5,0,1},{1,0,0,1}}};
 r.address=(uintptr_t)vertices;vertices[0].color[0]=NAN;failure(fd,&r,EINVAL);vertices[0].color[0]=1;
 r.flags=LR_GPU_CLEAR_BIT|LR_GPU_TRIANGLES_BIT;r.clear[3]=1;assert(ioctl(fd,LR_GPU_IOCTL,&r)==0);
 r.operation=LR_GPU_READ;r.flags=0;r.bytes=32*32*4;r.address=1;failure(fd,&r,EFAULT);
 char *guard=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);
 r.address=(uintptr_t)guard;failure(fd,&r,EFAULT);assert(!munmap(guard,4096));
 unsigned char pixels[4096];r.address=(uintptr_t)pixels;assert(ioctl(fd,LR_GPU_IOCTL,&r)==0);assert(pixels[3]==255);
 r.operation=LR_GPU_INFO;assert(ioctl(fd,LR_GPU_IOCTL,&r)==0&&r.submissions==1&&r.readbacks==3);
 r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_TRANSFER_INFO};assert(!ioctl(fd,LR_GPU_IOCTL,&r)&&r.count==1&&r.bytes==4096);r.flags=1;failure(fd,&r,EINVAL);
 r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_DRAW,.flags=LR_GPU_TRIANGLES_BIT,.count=4095,.bytes=4095*sizeof(struct lr_gpu_vertex),.address=1};failure(fd,&r,EFAULT);
 r.operation=LR_GPU_TRANSFER_INFO;r.flags=0;assert(!ioctl(fd,LR_GPU_IOCTL,&r)&&r.count==2&&r.bytes==4095*sizeof(struct lr_gpu_vertex));
 r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_READ,.width=32,.height=32,.bytes=sizeof pixels,.address=(uintptr_t)pixels};assert(!ioctl(fd,LR_GPU_IOCTL,&r)&&pixels[3]==255);
 r.operation=LR_GPU_TRANSFER_INFO;assert(!ioctl(fd,LR_GPU_IOCTL,&r)&&r.count==2&&r.bytes==4095*sizeof(struct lr_gpu_vertex));
 assert(!close(fd));puts("GPU_DEVICE_PASS validation=PASS unsupported=PASS guest_fault=PASS ownership=PASS");return 0;
}
