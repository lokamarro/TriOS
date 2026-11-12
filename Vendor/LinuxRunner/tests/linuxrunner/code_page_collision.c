// Exercise the actual block registry, including both boundary-page indexes.
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "asbestos/asbestos.c"
struct notes { struct asbestos *a; unsigned thread; };
static void *note_pages(void *opaque) {
 struct notes *n=opaque;
 for(unsigned i=0;i<4096;i++)
  code_seen_note(n->a, ((page_t)1<<32) + i*8 + n->thread);
 return NULL;
}
static struct fiber_block *insert(struct asbestos *a, page_t page, bool spanning) {
 struct fiber_block *b=calloc(1,sizeof *b);
 b->addr=(addr_t)page<<PAGE_BITS;b->end_addr=b->addr+(spanning?PAGE_SIZE+4:4);
 for(int i=0;i<2;i++)list_init(&b->jumps_from[i]);
 fiber_insert(a,b);return b;
}
int main(void) {
 struct mmu mmu={0};struct asbestos *a=asbestos_new(&mmu);
 page_t p=0x123, collision=p+FIBER_PAGE_HASH_SIZE;
 while(collision % FIBER_PAGE_HASH_SIZE != p % FIBER_PAGE_HASH_SIZE)collision++;
 page_t end_collision=p+2+FIBER_PAGE_HASH_SIZE;
 while(end_collision % FIBER_PAGE_HASH_SIZE != (p+2) % FIBER_PAGE_HASH_SIZE)end_collision++;
 struct fiber_block *one=insert(a,p,false),*two=insert(a,p+1,true);
 unsigned gen=atomic_load(&a->invalidate_gen);
 asbestos_invalidate_page(a,collision);
 assert(!one->is_jetsam&&!two->is_jetsam&&a->num_blocks==2);
 assert(atomic_load(&a->invalidate_gen)==gen);
 asbestos_invalidate_page(a,end_collision);
 assert(!two->is_jetsam); // end-page bucket collision also must not invalidate
 asbestos_invalidate_page(a,p+2);
 assert(two->is_jetsam&&!one->is_jetsam&&a->num_blocks==1);
 asbestos_invalidate_page(a,p);
 assert(one->is_jetsam&&a->num_blocks==0);
 // An insertion must invalidate a cached negative lookup, including end pages.
 one=insert(a,p,false);asbestos_invalidate_page(a,p);assert(one->is_jetsam);
 two=insert(a,p+1,true);asbestos_invalidate_page(a,p+2);assert(two->is_jetsam);
 one=insert(a,p,false);two=insert(a,p+5000,true);
 struct fiber_block *outside=insert(a,p+9000,false);
 asbestos_invalidate_range(a,p+1,p+5002);
 assert(!one->is_jetsam&&two->is_jetsam&&!outside->is_jetsam);
 asbestos_invalidate_range(a,0,(page_t)1<<32);
 assert(one->is_jetsam&&outside->is_jetsam&&a->num_blocks==0);
 // Exact-cache slot collision and both boundary-page insertions invalidate
 // only the corresponding negative entry, never newly inserted code.
 for(unsigned k=0;k<64;k++)asbestos_invalidate_page(a,p+1024*k);
 one=insert(a,p,false);two=insert(a,p+1023,true);
 asbestos_invalidate_page(a,p);assert(one->is_jetsam&&!two->is_jetsam);
 asbestos_invalidate_page(a,p+1024);assert(two->is_jetsam);
 asbestos_free(a);puts("CODE_PAGE_COLLISION_PASS");
 // Concurrent CLONE_VM decoders must never lose one another's filter bits.
 a=asbestos_new(&mmu);assert(!code_seen_maybe(a,(page_t)1<<32));
 pthread_t threads[8];struct notes n[8];
 for(unsigned i=0;i<8;i++){n[i]=(struct notes){a,i};assert(!pthread_create(&threads[i],NULL,note_pages,&n[i]));}
 for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
 for(unsigned i=0;i<32768;i++)assert(code_seen_maybe(a,((page_t)1<<32)+i));
 // Clearing block lists must not clear a bit another decoder might need.
 asbestos_invalidate_all(a);
 assert(code_seen_maybe(a,(page_t)1<<32));asbestos_free(a);
 puts("CODE_SEEN_CONCURRENCY_PASS");
}
