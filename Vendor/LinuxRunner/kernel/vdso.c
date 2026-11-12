#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/elf.h"
#include "kernel/vdso.h"

// vdso_data (the guest vDSO image) is generated from libvdso.so.elf by
// vdso/arm64/embed.py, so the build tracks the vDSO's contents.

int vdso_symbol(const char *name) {
    struct elf_header64 *header = (void *) vdso_data;
    struct prg_header64 *ph = (void *) ((char *) header + header->prghead_off);

    // find the PT_DYNAMIC section
    struct dyn_ent64 *dyn = NULL;
    for (int i = 0; i < header->phent_count; i++) {
        if (ph[i].type == PT_DYNAMIC) {
            dyn = (void *) ((char *) header + ph[i].offset);
            break;
        }
    }
    if (dyn == NULL)
        goto fail;

    // grab pointers to the symbols and the strings
    char *strings = NULL;
    struct elf_sym64 *syms = NULL;
    uint32_t *hash = NULL;
    for (; dyn->tag != DT_NULL; dyn++) {
        void *p = (char *) header + dyn->val;
        if (dyn->tag == DT_STRTAB)
            strings = p;
        else if (dyn->tag == DT_SYMTAB)
            syms = p;
        else if (dyn->tag == DT_HASH)
            hash = p;
    }
    if (strings == NULL || syms == NULL || hash == NULL)
        goto fail;

    // conveniently enough, the hashtable includes the number of symbols, which doesn't seeem to be anywhere else
    // https://flapenguin.me/2017/04/24/elf-lookup-dt-hash/
    int num_syms = hash[1];
    for (int i = 0; i < num_syms; i++) {
        char *sym_name = strings + syms[i].name;
        if (strcmp(name, sym_name) == 0)
            return (addr_t) syms[i].value;
    }
    return 0; // symbol not found

fail:
    return 0;
}
