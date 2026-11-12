// Compare actual production block invalidation; no stub registry.
#include <stdio.h>
#include <time.h>
#include "asbestos/asbestos.c"
int main(int argc,char **argv){unsigned working_set=argc>1?atoi(argv[1]):4;assert(working_set&&working_set<=256);struct mmu mmu={0};struct asbestos *a=asbestos_new(&mmu);for(unsigned k=0;k<2000;k++){
 struct fiber_block *b=calloc(1,sizeof *b);b->addr=(addr_t)(0x10000+k*1024)*4096;b->end_addr=b->addr+4;for(int j=0;j<2;j++)list_init(&b->jumps_from[j]);fiber_insert(a,b);
 }struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);for(unsigned i=0;i<100000;i++)asbestos_invalidate_page(a,0x10000+1024*(2000+(i%working_set)));clock_gettime(CLOCK_MONOTONIC,&end);printf("negative_cache_seconds %.9f blocks=%zu\n",end.tv_sec-begin.tv_sec+(end.tv_nsec-begin.tv_nsec)*1e-9,a->num_blocks);asbestos_free(a);}
