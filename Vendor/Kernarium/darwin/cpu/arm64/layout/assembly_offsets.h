/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Neutral, assembly-visible ARM64 layout contract.  Both translated engines
 * consume this file; assembly_layout.hpp verifies every relevant offset
 * against the C++ CpuState and memory::Tlb definitions, every event code
 * a gadget returns against EngineEvent, and the FPCR masks against their C++
 * constants, at compile time.
 */
#define UML_ARM64_ASSEMBLY_LAYOUT_VERSION 1
#define UML_GADGET_CPU_X0 0
#define UML_GADGET_CPU_SP 248
#define UML_GADGET_CPU_PC 256
#define UML_GADGET_CPU_NZCV 264
#define UML_GADGET_CPU_V0 272
#define UML_GADGET_CPU_FPCR 784
#define UML_GADGET_CPU_FPSR 788
#define UML_GADGET_CPU_TLS 792
#define UML_GADGET_CPU_TBI_ENABLED 800
#define UML_GADGET_CPU_PSTATE_DIT 801
#define UML_GADGET_CPU_PSTATE_DAIF_ACCESS 802
#define UML_GADGET_CPU_PSTATE_DAIF 803
#define UML_GADGET_CPU_EXCLUSIVE_ADDRESS 808
#define UML_GADGET_CPU_EXCLUSIVE_KEY 816
#define UML_GADGET_CPU_EXCLUSIVE_VALUE 824
#define UML_GADGET_CPU_EXCLUSIVE_VALUE2 832
#define UML_GADGET_CPU_EXCLUSIVE_VERSION 840
#define UML_GADGET_CPU_EXCLUSIVE_SIZE 848
/* The bit of exclusive_key that marks a gadget engine re-entry. */
#define UML_GADGET_EXCLUSIVE_KEY_REENTERED_BIT 63
#define UML_GADGET_CPU_DEFERRED_VECTORS 856
#define UML_GADGET_CPU_DEFERRED_FPCR 864
#define UML_GADGET_CPU_DEFERRED_FPSR 868
#define UML_GADGET_CPU_FP_LOADED 872
#define UML_GADGET_CPU_FP_DIRTY 873
#define UML_GADGET_CPU_ZERO 880
#define UML_GADGET_CPU_HOST_FPCR 888
#define UML_GADGET_CPU_HOST_FPSR 896
#define UML_GADGET_CPU_FP_ACTIVE 904
#define UML_GADGET_CPU_INDIRECT_MISS 912
#define UML_GADGET_INDIRECT_ENTRIES 2
#define UML_GADGET_RETURN_STACK_ENTRIES 16
#define UML_GADGET_CPU_RETURN_STACK 920
#define UML_GADGET_CPU_RETURN_TOP (920 + 16 * UML_GADGET_RETURN_STACK_ENTRIES)
#define UML_GADGET_CPU_RETURN_MASK (UML_GADGET_RETURN_STACK_ENTRIES - 1)
#define UML_GADGET_CPU_TARGET_TIER 1184
#define UML_GADGET_TARGET_TIER_BITS 10

/* SME state (cpu/arm64/sme_profile.hpp), for both engines. The register file
 * lies behind UML_ARM64_CPU_SME_REGISTERS and is sized for the SVL in
 * UML_ARM64_CPU_SME_SVL: Z_n at n * SVL, P_n at
 * UML_ARM64_SME_P_OFFSET_VL * SVL + n * SVL / 8, ZA row r at
 * (UML_ARM64_SME_ZA_OFFSET_VL + r) * SVL, ZT0 after the SVL rows. */
#define UML_ARM64_CPU_TPIDR2 1192
#define UML_ARM64_CPU_SVCR 1200
#define UML_ARM64_CPU_SME_LOADED 1201
#define UML_ARM64_CPU_SME_DIRTY 1202
#define UML_ARM64_CPU_SME_HOST_MODE 1203
#define UML_ARM64_CPU_SME_SVL 1204
#define UML_ARM64_CPU_SME_TASK 1208
#define UML_ARM64_CPU_SME_REGISTERS 1216
#define UML_ARM64_SVCR_SM 1
#define UML_ARM64_SVCR_ZA 2
#define UML_ARM64_SME_HOST_STREAMING 1
#define UML_ARM64_SME_HOST_ZA 2
#define UML_ARM64_SME_HOST_STREAMING_RESIDENT 4
#define UML_ARM64_SME_HOST_ZA_RESIDENT 8
#define UML_ARM64_SME_P_OFFSET_VL 32
#define UML_ARM64_SME_ZA_OFFSET_VL 34
#define UML_ARM64_SME_ZT0_BYTES 64

/* Bytes 1232 to 1507 are reserved (cpu/arm64/cpu_state.hpp): no engine
 * reads or writes them, and the offsets after them stay where they were. */

/* AArch32 EL0 state (cpu/arm64/aarch32/a32_profile.hpp), for both engines:
 * GE[3:0] widened to byte lanes, FPSCR.NZCV in bits 31:28, TPIDRURO, then the
 * Q flag, ITSTATE, CP15BEN and the AArch32 entry marker, one byte each. */
#define UML_ARM64_CPU_A32_GE_MASK 1508
#define UML_ARM64_CPU_A32_FPSCR_NZCV 1512
#define UML_ARM64_CPU_A32_TPIDRURO 1516
#define UML_ARM64_CPU_A32_Q 1520
#define UML_ARM64_CPU_A32_ITSTATE 1521
#define UML_ARM64_CPU_A32_CP15BEN 1522
#define UML_ARM64_CPU_A32 1523
#define UML_ARM64_CPU_A32_FP_STANDARD 1524

/* Context synchronization (cpu/arm64/cpu_state.hpp), for both engines: the
 * address of the running address space's code-change sequence, then the
 * value the running translations were validated at. */
#define UML_ARM64_CPU_CODE_SEQUENCE 1528
#define UML_ARM64_CPU_CODE_SEQUENCE_SEEN 1536

#define UML_GADGET_TLB_ENTRIES 0
#define UML_GADGET_TLB_ENTRY_READ_PAGE 0
#define UML_GADGET_TLB_ENTRY_READ_DATA_MINUS_ADDRESS 8
#define UML_GADGET_TLB_ENTRY_WRITE_PAGE 16
#define UML_GADGET_TLB_ENTRY_WRITE_DATA_MINUS_ADDRESS 24
/* The TLB (memory/tlb.hpp) is 16,384 one-way 32-byte entries indexed by the
 * guest page number. */
#define UML_GADGET_TLB_INDEX_BITS 14
#define UML_GADGET_GUEST_PAGE_SHIFT 12
#define UML_GADGET_TLB_SET_SHIFT 5
#define UML_GADGET_TLB_FAULT_ADDRESS 524304
#define UML_GADGET_TLB_FAULT_ACCESS 524312
#define UML_GADGET_TLB_UNALIGNED_ATOMICS 524321

#define UML_GADGET_EVENT_SYSCALL 1
#define UML_GADGET_EVENT_MEMORY_FAULT 2
#define UML_GADGET_EVENT_INTERRUPT 3
#define UML_GADGET_EVENT_ILLEGAL 4
#define UML_GADGET_EVENT_BREAKPOINT 5
#define UML_GADGET_EVENT_SME_ACCESS 14

/* FPCR bits a guest can set, and the trap enables among them. FIZ, AH and
 * NEP join them where the virtual CPU implements FEAT_AFP. */
#define UML_ARM64_FPCR_WRITABLE 0x07c89f00
#define UML_ARM64_FPCR_AFP_CONTROLS 0x7
#define UML_ARM64_FPCR_TRAP_ENABLES 0x9f00

/* FPSR bits a guest can set: QC and the cumulative exception flags. */
#define UML_ARM64_FPSR_WRITABLE 0x0800009f

/* Engine-neutral aliases for code that is not part of gadget translation. */
#define UML_ARM64_CPU_FP_ACTIVE UML_GADGET_CPU_FP_ACTIVE
#define UML_ARM64_CPU_FPSR UML_GADGET_CPU_FPSR
#define UML_ARM64_CPU_HOST_FPCR UML_GADGET_CPU_HOST_FPCR
#define UML_ARM64_CPU_HOST_FPSR UML_GADGET_CPU_HOST_FPSR
