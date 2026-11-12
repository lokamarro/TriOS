/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_ELF64_H
#define UML_DARWIN_ELF64_H

#include <stdint.h>

#define UML_EI_NIDENT 16

typedef struct {
	unsigned char ident[UML_EI_NIDENT];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
} uml_elf64_ehdr;

typedef struct {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
} uml_elf64_phdr;

typedef struct {
	int64_t tag;
	union {
		uint64_t value;
		uint64_t pointer;
	} un;
} uml_elf64_dyn;

typedef struct {
	uint32_t name;
	unsigned char info;
	unsigned char other;
	uint16_t shndx;
	uint64_t value;
	uint64_t size;
} uml_elf64_sym;

typedef struct {
	uint64_t offset;
	uint64_t info;
	int64_t addend;
} uml_elf64_rela;

#define UML_ELFMAG0 0x7f
#define UML_ELFMAG1 'E'
#define UML_ELFMAG2 'L'
#define UML_ELFMAG3 'F'
#define UML_ELFCLASS64 2
#define UML_ELFDATA2LSB 1
#define UML_ET_DYN 3
#define UML_EM_AARCH64 183

#define UML_PT_LOAD 1
#define UML_PT_DYNAMIC 2

#define UML_PF_X 1
#define UML_PF_W 2
#define UML_PF_R 4

#define UML_DT_NULL 0
#define UML_DT_PLTRELSZ 2
#define UML_DT_SYMTAB 6
#define UML_DT_STRTAB 5
#define UML_DT_RELA 7
#define UML_DT_RELASZ 8
#define UML_DT_RELAENT 9
#define UML_DT_STRSZ 10
#define UML_DT_SYMENT 11
#define UML_DT_PLTREL 20
#define UML_DT_JMPREL 23
#define UML_DT_RELR 36
#define UML_DT_RELRSZ 35
#define UML_DT_RELRENT 37

#define UML_DT_RELA_VALUE 7

#define UML_SHN_UNDEF 0
#define UML_STB_WEAK 2
#define UML_ELF64_ST_BIND(info) ((info) >> 4)
#define UML_ELF64_R_SYM(info) ((uint32_t)((info) >> 32))
#define UML_ELF64_R_TYPE(info) ((uint32_t)(info))

#define UML_R_AARCH64_ABS64 257
#define UML_R_AARCH64_GLOB_DAT 1025
#define UML_R_AARCH64_JUMP_SLOT 1026
#define UML_R_AARCH64_RELATIVE 1027

#endif
