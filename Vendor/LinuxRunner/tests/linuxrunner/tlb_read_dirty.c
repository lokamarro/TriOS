// Real TLB misses must preserve pending write invalidation. A read of code
// must not evict its decoded blocks merely because the translation was cold.
#include <assert.h>
#include <stdio.h>
#include "asbestos/asbestos.c"

static unsigned char bytes[3 * PAGE_SIZE];
static unsigned denied;
static void *translate(struct mmu *mmu, addr_t addr, int type) {
    (void)mmu;
    if (addr < 0x1000 || addr >= 0x4000 ||
        (denied & (1u << (PAGE(addr) - 1))) ||
        (type == MEM_WRITE && addr == 0x3000)) return NULL;
    return bytes + addr - 0x1000;
}
static void *write_nofault(struct mmu *mmu, addr_t addr) {
    return translate(mmu, addr, MEM_WRITE);
}
static struct fiber_block *code(struct asbestos *a, addr_t addr) {
    struct fiber_block *b = calloc(1, sizeof(*b));
    b->addr = addr; b->end_addr = addr + 4;
    for (int i = 0; i < 2; i++) list_init(&b->jumps_from[i]);
    fiber_insert(a, b);
    return b;
}
static void boundary(struct tlb *tlb) {
    if (tlb->dirty_page != TLB_PAGE_EMPTY) {
        asbestos_invalidate_page(tlb->mmu->asbestos, PAGE(tlb->dirty_page));
        tlb->dirty_page = TLB_PAGE_EMPTY;
    }
}
int main(void) {
    struct mmu_ops ops = {.translate = translate,
                         .translate_write_nofault = write_nofault};
    struct mmu mmu = {.ops = &ops, .changes = 1};
    mmu.asbestos = asbestos_new(&mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    tlb_refresh(tlb, &mmu);
    struct fiber_block *a = code(mmu.asbestos, 0x1000);
    struct fiber_block *b = code(mmu.asbestos, 0x2000);
    unsigned gen = atomic_load(&mmu.asbestos->invalidate_gen);
    uint32_t value = 0x12345678, out = 0;
    memcpy(bytes, &value, sizeof(value));
    assert(tlb_read(tlb, 0x1000, &out, sizeof(out)) && out == value);
    boundary(tlb);
    assert(!a->is_jetsam && !b->is_jetsam);
    assert(atomic_load(&mmu.asbestos->invalidate_gen) == gen);

    // A subsequent cold read must not overwrite the earlier write marker.
    assert(tlb_write(tlb, 0x1000, &value, sizeof(value)));
    assert(tlb_read(tlb, 0x2000, &out, sizeof(out)));
    boundary(tlb);
    assert(a->is_jetsam && !b->is_jetsam);
    // Check a write miss as well as the speculative-writable hit above.
    tlb_flush(tlb);
    assert(tlb_write(tlb, 0x2000, &value, sizeof(value)));
    assert(tlb_read(tlb, 0x3000, &out, sizeof(out)));
    boundary(tlb);
    assert(b->is_jetsam);

    tlb_flush(tlb);
    assert(tlb_read(tlb, 0x1ffe, &out, sizeof(out)));
    assert(tlb->dirty_page == TLB_PAGE_EMPTY);
    denied = 2; mmu.changes++; tlb_flush(tlb);
    assert(!tlb_read(tlb, 0x1ffe, &out, sizeof(out)));
    assert(tlb->segfault_addr == 0x2000 && tlb->dirty_page == TLB_PAGE_EMPTY);
    denied = 0; mmu.changes++; tlb_flush(tlb);
    assert(!tlb_write(tlb, 0x3000, &value, sizeof(value)));
    assert(tlb->segfault_addr == 0x3000 && tlb->dirty_page == TLB_PAGE_EMPTY);
    assert(!tlb_read(tlb, 0x4000, &out, sizeof(out)));
    assert(tlb->segfault_addr == 0x4000 && tlb->dirty_page == TLB_PAGE_EMPTY);
    tlb_free(tlb); asbestos_free(mmu.asbestos);
    puts("TLB_READ_DIRTY_PASS");
}
