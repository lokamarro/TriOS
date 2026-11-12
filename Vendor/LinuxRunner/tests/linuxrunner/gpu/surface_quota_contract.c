#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define LR_GPU_GUEST
#include "guest_gpu.h"
static int create(unsigned w,unsigned h){int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request r={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=w,.height=h};if(ioctl(fd,LR_GPU_IOCTL,&r)){int error=errno;close(fd);errno=error;return -1;}return fd;}
static int resize(int fd,unsigned w,unsigned h){struct lr_gpu_request r={.version=LR_GPU_VERSION,.operation=LR_GPU_RESIZE,.width=w,.height=h};return ioctl(fd,LR_GPU_IOCTL,&r);}
// Metal default surfaces now reserve 4-byte color + 4-byte depth + 1-byte stencil.
// Seven 1024-square targets fit 64 MiB; an eighth must fail. Shrinking
// returns the exact charge, including stencil, and failed growth preserves it.
int main(void){int fds[7];for(int i=0;i<7;i++){fds[i]=create(1024,1024);assert(fds[i]>=0);}assert(create(1024,1024)==-1&&errno==ENOMEM);close(fds[6]);fds[6]=create(1024,1024);assert(fds[6]>=0);assert(!resize(fds[0],1024,512));int extra=create(1024,512);assert(extra>=0);assert(resize(fds[0],1024,1024)==-1&&errno==ENOMEM);struct lr_gpu_framebuffer fb={.command=LR_GPU_FB_QUERY};struct lr_gpu_request request={.version=LR_GPU_VERSION,.operation=LR_GPU_FRAMEBUFFER,.address=(uintptr_t)&fb,.bytes=sizeof fb};assert(!ioctl(fds[0],LR_GPU_IOCTL,&request)&&fb.width==1024&&fb.height==512);close(extra);assert(!resize(fds[0],1024,1024));for(int i=0;i<7;i++)close(fds[i]);int recovered=create(1024,1024);assert(recovered>=0);close(recovered);puts("SURFACE_QUOTA_CONTRACT_PASS global_64MiB=PASS create_recovery=PASS shrink=PASS failed_resize_preserved=PASS teardown=PASS");}
