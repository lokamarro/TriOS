/* Reproduce the vector-owned by-value RNG behavior independently of Qt.
 * Counting generator is only a test oracle, never used by production RNG. */
#include <cstdint>
#include <cstdio>
#include <vector>
#include <limits>
static uint64_t calls,bytes;
struct Buffer {std::vector<uint32_t> data;size_t index=0;explicit Buffer(size_t n):data(n){} uint32_t next(){if(index==data.size())index=0;if(!index){++calls;bytes+=data.size()*4;for(size_t i=0;i<data.size();++i)data[i]=uint32_t(i+1);}return data[index++];}};
static int select_ref(int count,Buffer &b){uint32_t max=UINT32_MAX/count*count;for(;;){uint32_t x=b.next();if(x<max)return x%count;}}
static int select_copy(int count,Buffer b){return select_ref(count,b);}
int main(){Buffer b(45);for(int i=0;i<45;i++)if(select_copy(128,b)!=1)return 1;if(calls!=45||bytes!=8100||b.index!=0)return 2;std::printf("RANDOM_BUFFER_COPY: by-value calls=%llu bytes=%llu source_index=%zu\n",(unsigned long long)calls,(unsigned long long)bytes,b.index);calls=bytes=0;for(int i=0;i<45;i++)if(select_ref(128,b)!=i+1)return 3;if(calls!=1||bytes!=180||b.index!=45)return 4;std::puts("RANDOM_BUFFER_COPY: PASS reference reuse=1 fill; text-spoiler 9000x5x180000=8100000000 bytes by-value (derived, not timed)");}
