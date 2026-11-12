#include "guest_gpu.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
static int request(int fd,unsigned op,void *p,size_t bytes,unsigned format,unsigned index){struct lr_gpu_request r={.version=LR_GPU_VERSION,.operation=op,.width=32,.height=32,.address=(uintptr_t)p,.bytes=bytes,.flags=format,.count=index};return ioctl(fd,LR_GPU_IOCTL,&r);}
int main(void){int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request c={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=32,.height=32};assert(!ioctl(fd,LR_GPU_IOCTL,&c));
 size_t header=sizeof(struct lr_gpu_mrt),bytes=header+32*32*4;struct lr_gpu_mrt *p=calloc(1,bytes);assert(p);p->count=1;p->color[0].format=0x8058;p->color[0].mask=15;p->color[0].offset=header;p->color[0].bytes=32*32*4;memset((char *)p+header,117,bytes-header);assert(!request(fd,LR_GPU_IMPORT_MRT,p,bytes,0,0));unsigned char pixels[32*32*4];assert(!request(fd,LR_GPU_READ_MRT,pixels,sizeof pixels,0x8058,0));assert(!memcmp(pixels,(char *)p+header,sizeof pixels));
 p->count=5;assert(request(fd,LR_GPU_IMPORT_MRT,p,bytes,0,0)==-1&&errno==EINVAL);p->count=1;p->color[0].offset++;assert(request(fd,LR_GPU_IMPORT_MRT,p,bytes,0,0)==-1&&errno==EINVAL);p->color[0].offset--;p->color[0].mask=16;assert(request(fd,LR_GPU_IMPORT_MRT,p,bytes,0,0)==-1&&errno==EINVAL);p->color[0].mask=15;p->color[0].format=0;assert(request(fd,LR_GPU_IMPORT_MRT,p,bytes,0,0)==-1&&errno==EINVAL);p->color[0].format=0x8058;
 assert(request(fd,LR_GPU_READ_MRT,pixels,sizeof pixels,0x8058,1)==-1&&errno==EINVAL);assert(request(fd,LR_GPU_READ_MRT,pixels,sizeof pixels-1,0x8058,0)==-1&&errno==EINVAL);
 void *guard=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);assert(request(fd,LR_GPU_IMPORT_MRT,guard,bytes,0,0)==-1&&errno==EFAULT);assert(request(fd,LR_GPU_READ_MRT,guard,sizeof pixels,0x8058,0)==-1&&errno==EFAULT);
 float depth[32*32],actual[32*32];for(unsigned i=0;i<32*32;i++)depth[i]=(float)i/(32*32);assert(!request(fd,LR_GPU_IMPORT_DEPTH,depth,sizeof depth,0,0));assert(!request(fd,LR_GPU_READ_DEPTH,actual,sizeof actual,0,0));assert(!memcmp(depth,actual,sizeof depth));depth[0]=NAN;assert(request(fd,LR_GPU_IMPORT_DEPTH,depth,sizeof depth,0,0)==-1&&errno==EINVAL);depth[0]=2;assert(request(fd,LR_GPU_IMPORT_DEPTH,depth,sizeof depth,0,0)==-1&&errno==EINVAL);assert(request(fd,LR_GPU_IMPORT_DEPTH,guard,sizeof depth,0,0)==-1&&errno==EFAULT);depth[0]=0;assert(!request(fd,LR_GPU_IMPORT_DEPTH,depth,sizeof depth,0,0));
 // Failed requests leave the previous color layout and device usable.
 assert(!request(fd,LR_GPU_READ_MRT,pixels,sizeof pixels,0x8058,0));assert(!memcmp(pixels,(char *)p+header,sizeof pixels));munmap(guard,4096);free(p);close(fd);puts("INTEROP_TRANSFER_CONTRACT_PASS MRT_pixels=PASS invalid_layout=PASS guarded_pointers=PASS depth_roundtrip=PASS invalid_depth=PASS recovery=PASS cleanup=PASS");return 0;}
