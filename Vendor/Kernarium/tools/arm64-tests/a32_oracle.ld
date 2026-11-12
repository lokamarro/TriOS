/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The AArch32 corpus oracle is one read-write-execute segment, linked just
 * below the code region it launches programs in (0x4f000000) so that the
 * launch is a direct branch with no register.
 */
ENTRY(_start)
PHDRS { image PT_LOAD FILEHDR PHDRS FLAGS(7); }
SECTIONS
{
	. = 0x4e800000 + SIZEOF_HEADERS;
	.text : { *(.text .text.*) } :image
	.rodata : { *(.rodata .rodata.*) } :image
	.data : { *(.data .data.*) } :image
	.bss : { *(.bss .bss.* COMMON) } :image
	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.eh_frame*) }
}
