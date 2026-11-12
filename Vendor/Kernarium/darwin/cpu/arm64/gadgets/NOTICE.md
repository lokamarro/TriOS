# Design credit: iSH

The gadget engines in this directory and in `../../x86_64/gadgets` are this
project's own code, licensed GPL-2.0-or-later as each file's SPDX header says.
No text from iSH remains in either.

Their design was inspired by iSH, created by Theodore Dubois and the iSH
contributors. Its "asbestos" engine (formerly `jit/gadgets-aarch64`) runs x86
guest code on an arm64 host without generating machine code: a translated
block is an array of pointers to precompiled routines with their operands
inline, and guest memory is reached through a software TLB. The engines here
took that shape. Everything else is ours: the A64, A32, T32 and x86-64
decoders and gadget banks, the TLB geometry, the register contracts, the
block linking and the translation cache.

Runtime blocks remain ordinary non-executable data.
