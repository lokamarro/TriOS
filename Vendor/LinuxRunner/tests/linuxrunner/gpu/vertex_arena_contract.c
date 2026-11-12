// Linux guest ABI benchmark and queued-draw lifetime regression.
#define LR_GPU_GUEST 1
#include "guest_gpu.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv){int baseline=argc>1&&!strcmp(argv[1],"--baseline");unsigned frames=argc>2?atoi(argv[2]):20;assert(frames&&frames<=1000);int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);
 struct lr_gpu_request r={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=32,.height=32};assert(!ioctl(fd,LR_GPU_IOCTL,&r));
 struct lr_gpu_vertex vertices[288];unsigned char pixels[4096];double start=now();
 for(unsigned frame=0;frame<frames;frame++){
  r=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_DRAW,.flags=LR_GPU_CLEAR_BIT,.clear={0,0,0,1}};assert(!ioctl(fd,LR_GPU_IOCTL,&r));
  for(unsigned draw=0;draw<128;draw++){
   float x=draw%2?0:-1;float positions[6][2]={{x,-1},{x+1,-1},{x,1},{x,1},{x+1,-1},{x+1,1}};
   for(unsigned i=0;i<288;i++){vertices[i]=(struct lr_gpu_vertex){.position={positions[i%6][0],positions[i%6][1],0,1},.color={draw%2?0:1,draw%2?1:0,0,1}};}
   r=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_DRAW,.flags=LR_GPU_TRIANGLES_BIT,.count=288,.bytes=sizeof vertices,.address=(uintptr_t)vertices};assert(!ioctl(fd,LR_GPU_IOCTL,&r));
  }
  r=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_READ,.width=32,.height=32,.bytes=sizeof pixels,.address=(uintptr_t)pixels};assert(!ioctl(fd,LR_GPU_IOCTL,&r));
  assert(pixels[(16*32+8)*4]==255&&pixels[(16*32+8)*4+1]==0&&pixels[(16*32+24)*4]==0&&pixels[(16*32+24)*4+1]==255);
 }
 double elapsed=now()-start;
 if(!baseline){r=(struct lr_gpu_request){.version=LR_GPU_VERSION,.operation=LR_GPU_RESOURCE_INFO};assert(!ioctl(fd,LR_GPU_IOCTL,&r));assert(r.count==1&&r.bytes==(uint64_t)frames*128*sizeof vertices);printf("VERTEX_ARENA_PASS elapsed_seconds=%.6f draws=%u Metal_vertex_allocations=%u vertex_uploaded_bytes=%llu queued_pixels=PASS\n",elapsed,frames*128,r.count,(unsigned long long)r.bytes);}
 else printf("VERTEX_BASELINE_PASS elapsed_seconds=%.6f draws=%u queued_pixels=PASS\n",elapsed,frames*128);
 close(fd);return 0;
}
