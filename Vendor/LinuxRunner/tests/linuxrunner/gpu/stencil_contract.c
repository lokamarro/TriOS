#define LR_GPU_GUEST 1
#include "guest_gpu.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
_Static_assert(sizeof(struct lr_gpu_raster_state)==40,"preserve raster ABI");
_Static_assert(sizeof(struct lr_gpu_vertex)==32,"preserve vertex ABI");
static unsigned w=37,h=29; // Deliberately not a 256-byte-aligned row.
static int req(int fd,unsigned op,void *p,size_t bytes){struct lr_gpu_request r={.version=1,.operation=op,.width=w,.height=h,.bytes=bytes,.address=(uintptr_t)p};return ioctl(fd,LR_GPU_IOCTL,&r);}
static void state(int fd,struct lr_gpu_stencil_state *s){assert(!req(fd,LR_GPU_STENCIL_STATE,s,sizeof *s));}
static void draw(int fd,unsigned flags,float red,float green,float blue){struct lr_gpu_vertex v[3]={{{-1,-1,0,1},{red,green,blue,1}},{{1,-1,0,1},{red,green,blue,1}},{{0,1,0,1},{red,green,blue,1}}};struct lr_gpu_request r={.version=1,.operation=LR_GPU_DRAW,.flags=LR_GPU_TRIANGLES_BIT|flags,.count=3,.bytes=sizeof v,.address=(uintptr_t)v,.clear={0,0,0,1}};assert(!ioctl(fd,LR_GPU_IOCTL,&r));}
int main(void){int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);assert(!req(fd,LR_GPU_CREATE,NULL,0));unsigned char in[37*29],out[37*29],pixels[37*29*4];for(unsigned i=0;i<sizeof in;i++)in[i]=(i*17+i/37)&255;
 assert(!req(fd,LR_GPU_IMPORT_STENCIL,in,sizeof in));assert(!req(fd,LR_GPU_READ_STENCIL,out,sizeof out));assert(!memcmp(in,out,sizeof in));
 struct lr_gpu_stencil_state s={.enabled=1,.clear=1,.clear_value=0};s.front=s.back=(struct lr_gpu_stencil_face){.function=0x0207,.reference=7,.read_mask=255,.write_mask=255,.fail=0x1e00,.depth_fail=0x1e00,.depth_pass=0x1e01};state(fd,&s);s.clear=0;
 draw(fd,LR_GPU_CLEAR_BIT,1,0,0);assert(!req(fd,LR_GPU_READ_STENCIL,out,sizeof out));unsigned center=(h/2)*w+w/2;printf("STENCIL_WRITE center=%u outside=%u\n",out[center],out[(h-1)*w]);fflush(stdout);assert(out[center]==7&&out[(h-1)*w]==0);
 s.front.function=s.back.function=0x0202;s.front.depth_pass=s.back.depth_pass=0x1e00;state(fd,&s);draw(fd,0,0,1,0);assert(!req(fd,LR_GPU_READ,pixels,w*h*4));assert(pixels[center*4+1]==255);
 s.front.reference=s.back.reference=3;state(fd,&s);draw(fd,0,0,0,1);assert(!req(fd,LR_GPU_READ,pixels,w*h*4));assert(pixels[center*4+1]==255&&pixels[center*4+2]==0);
 // Same compare/depth, changed operation and write mask must not hit a stale cached descriptor.
 s.front.function=s.back.function=0x0207;s.front.reference=s.back.reference=0xa0;s.front.write_mask=s.back.write_mask=0xf0;s.front.depth_pass=s.back.depth_pass=0x1e01;state(fd,&s);draw(fd,0,1,0,0);assert(!req(fd,LR_GPU_READ_STENCIL,out,sizeof out));assert(out[center]==0xa7);
 // Stencil still runs when depth is disabled, and a depth clear does not clear stencil.
 draw(fd,LR_GPU_DEPTH_CLEAR_BIT,1,0,0);assert(!req(fd,LR_GPU_READ_STENCIL,out,sizeof out));assert(out[center]==0xa7);
 void *guard=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);assert(req(fd,LR_GPU_IMPORT_STENCIL,guard,sizeof in)<0&&errno==EFAULT);assert(req(fd,LR_GPU_READ_STENCIL,guard,sizeof in)<0&&errno==EFAULT);assert(req(fd,LR_GPU_IMPORT_STENCIL,in,sizeof in-1)<0&&errno==EINVAL);s.front.function=0;assert(req(fd,LR_GPU_STENCIL_STATE,&s,sizeof s)<0&&errno==EINVAL);
 int other=open("/dev/linuxrunner-gpu",O_RDWR);assert(other>=0);assert(!req(other,LR_GPU_CREATE,NULL,0));memset(in,13,sizeof in);assert(!req(other,LR_GPU_IMPORT_STENCIL,in,sizeof in));assert(!req(fd,LR_GPU_READ_STENCIL,out,sizeof out)&&out[center]==0xa7);close(other);
 w=19;h=11;assert(!req(fd,LR_GPU_RESIZE,NULL,0));memset(in,21,w*h);assert(!req(fd,LR_GPU_IMPORT_STENCIL,in,w*h));assert(!req(fd,LR_GPU_READ_STENCIL,out,w*h));assert(!memcmp(in,out,w*h));munmap(guard,4096);close(fd);puts("STENCIL_CONTRACT_PASS row_orientation=PASS write_test_read_pixels=PASS state_cache=PASS masks=PASS depth_clear=PASS invalid_requests=PASS isolation=PASS resize=PASS cleanup=PASS");}
