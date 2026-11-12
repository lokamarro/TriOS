#define LR_GPU_GUEST 1
#include "guest_gpu.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
static double now(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
    unsigned dimension=argc>1?(unsigned)atoi(argv[1]):32;
    assert(dimension==32 || dimension==512);
    unsigned reads=dimension==512?1000:10000;
    size_t bytes=(size_t)dimension*dimension*4;
    int fd = open("/dev/linuxrunner-gpu", O_RDWR | O_CLOEXEC); assert(fd >= 0);
    struct lr_gpu_request r = {.version=1,.operation=LR_GPU_CREATE,.width=dimension,.height=dimension};
    assert(!ioctl(fd, LR_GPU_IOCTL, &r));
    r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_DRAW,.flags=LR_GPU_CLEAR_BIT,.clear={1,0,0,1}};
    assert(!ioctl(fd, LR_GPU_IOCTL, &r));
    unsigned char *pixels=malloc(bytes);assert(pixels);
    r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_READ,.width=dimension,.height=dimension,.bytes=bytes,.address=(uintptr_t)pixels};
    assert(!ioctl(fd,LR_GPU_IOCTL,&r)); // Synchronize the initial draw before timing.
    double start=now();
    for(unsigned i=0;i<reads;i++) assert(!ioctl(fd,LR_GPU_IOCTL,&r));
    double elapsed=now()-start;
    assert(pixels[0]==255 && pixels[1]==0 && pixels[2]==0 && pixels[3]==255);
    r=(struct lr_gpu_request){.version=1,.operation=LR_GPU_TRANSFER_INFO};
    errno=0;int status=ioctl(fd,LR_GPU_IOCTL,&r);
    if(status==0) { assert(r.count==1 && r.bytes==bytes); }
    else assert(errno==EOPNOTSUPP); // Baseline lacks this diagnostic operation.
    printf("GPU_TRANSFER_BENCH seconds=%.9f reads=%u dimension=%u allocations=%s capacity=%llu\n",
           elapsed,reads,dimension,status==0?"1":"unavailable",status==0?(unsigned long long)r.bytes:0);
    assert(!close(fd));free(pixels);return 0;
}
