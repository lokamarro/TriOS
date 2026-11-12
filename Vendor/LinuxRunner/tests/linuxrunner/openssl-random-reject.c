#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern int RAND_bytes(unsigned char *, int);
int main(void) {
 alarm(25);
 uint32_t threshold;
 unsigned range=420;
 __asm__ volatile("mov w21, #-1\n\tudiv w21, w21, %w1\n\tmul w21, w21, %w1\n\tmov %w0, w21" : "=r"(threshold) : "r"(range) : "x21");
 if(threshold!=UINT32_MAX/range*range){fprintf(stderr,"RANDOM_REJECT: incorrect threshold=%u\n",threshold);return 1;}
 uint32_t *buf=malloc(249632);if(!buf)return 2;
 if(RAND_bytes((unsigned char*)buf,249632)!=1)return 3;
 unsigned accepted=0,zero=0,ones=0;uint32_t first=buf[0];
 for(unsigned i=0;i<62408;i++){accepted+=buf[i]<threshold;zero+=buf[i]==0;ones+=buf[i]==UINT32_MAX;}
 printf("RANDOM_REJECT: threshold=%u accepted=%u zero=%u allones=%u first=%08x\n",threshold,accepted,zero,ones,first);
 free(buf);if(accepted<62000||zero>100||ones>100)return 4;
 puts("RANDOM_REJECT: PASS");return 0;
}
