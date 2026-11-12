#include "guest_gpu.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
static void send(int fd,unsigned operation,void *data,size_t bytes){struct lr_gpu_request r={.version=LR_GPU_VERSION,.operation=operation,.address=(uintptr_t)data,.bytes=bytes};assert(!ioctl(fd,LR_GPU_IOCTL,&r));}
static void bad(int fd,unsigned operation,void *data,size_t bytes,int error){struct lr_gpu_request r={.version=LR_GPU_VERSION,.operation=operation,.address=(uintptr_t)data,.bytes=bytes};assert(ioctl(fd,LR_GPU_IOCTL,&r)==-1&&errno==error);}
int main(void){int fd=open("/dev/linuxrunner-gpu",O_RDWR);assert(fd>=0);struct lr_gpu_request create={.version=LR_GPU_VERSION,.operation=LR_GPU_CREATE,.width=32,.height=32};assert(!ioctl(fd,LR_GPU_IOCTL,&create));
 struct lr_gl_compile_v1 old={.name=1,.stage=0x8b31};strcpy(old.source,"#version 100\nvoid main(){gl_Position=vec4(0,0,0,1);}");old.length=strlen(old.source);send(fd,LR_GPU_COMPILE_SHADER,&old,sizeof old);assert(old.status);struct lr_gl_compile *now=calloc(1,sizeof *now);assert(now);now->name=2;now->stage=0x8b30;strcpy(now->source,"#version 100\nprecision mediump float;void main(){gl_FragColor=vec4(0,1,0,1);}");now->length=strlen(now->source);send(fd,LR_GPU_COMPILE_SHADER_EXTENDED,now,sizeof *now);assert(now->status);
 bad(fd,LR_GPU_COMPILE_SHADER_EXTENDED,&old,sizeof old,EINVAL);bad(fd,LR_GPU_COMPILE_SHADER,now,sizeof *now,EINVAL);now->length=LR_GL_MAX_SOURCE;bad(fd,LR_GPU_COMPILE_SHADER_EXTENDED,now,sizeof *now,EINVAL);struct lr_gl_link linked={.name=1,.vertex=1,.fragment=2};send(fd,LR_GPU_LINK_PROGRAM,&linked,sizeof linked);assert(linked.status);
 struct lr_gl_blocks_v1 legacy={.program=1};send(fd,LR_GPU_QUERY_BLOCKS,&legacy,sizeof legacy);assert(!legacy.count);struct lr_gl_blocks *blocks=calloc(1,sizeof *blocks);assert(blocks);blocks->program=1;send(fd,LR_GPU_QUERY_BLOCKS_EXTENDED,blocks,sizeof *blocks);assert(!blocks->count);bad(fd,LR_GPU_QUERY_BLOCKS_EXTENDED,&legacy,sizeof legacy,EINVAL);
 struct lr_gl_storages storage={.program=1};send(fd,LR_GPU_QUERY_STORAGES,&storage,sizeof storage);assert(!storage.count);struct lr_gl_uniform_extents extents={.program=1};send(fd,LR_GPU_QUERY_UNIFORM_EXTENTS,&extents,sizeof extents);assert(!extents.count);
 void *guard=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);bad(fd,LR_GPU_QUERY_STORAGES,guard,sizeof storage,EFAULT);bad(fd,LR_GPU_COMPILE_SHADER_EXTENDED,guard,sizeof *now,EFAULT);struct lr_gl_uniform_array_data array={.program=1,.index=16,.count=1};bad(fd,LR_GPU_UNIFORM_ARRAY_DATA,&array,sizeof array,EINVAL);struct lr_gl_storage_data packet={.program=1,.count=1};packet.block[0].offset=sizeof packet;packet.block[0].bytes=LR_GL_MAX_STORAGE_BYTES+1;bad(fd,LR_GPU_STORAGE_DATA,&packet,sizeof packet,EINVAL);bad(fd,LR_GPU_DRAW_INDEXED_BASE,&packet,sizeof packet,EINVAL);munmap(guard,4096);free(blocks);free(now);close(fd);puts("EXTENDED_ABI_CONTRACT_PASS legacy_compile=PASS legacy_reflection=PASS new_layouts=PASS invalid_sizes=PASS bounds=PASS guarded_pointers=PASS cleanup=PASS");return 0;}
