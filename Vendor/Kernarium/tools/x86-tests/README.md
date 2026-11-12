# x86 instruction unit-corpus producer

`build-unit-corpus.py` produces new independently validated x86 instruction
cases for promotion to the checked-in source tables. It normalizes the pinned K
and FEX cases and the repository's own cases, fills missing or disputed K
states from a provenanced native-Intel trace, and accepts a case only when its
declared final state is the native one. It never converts an illegal
instruction into an exclusion. The freestanding `native-k-trace.cpp` helper is
that native oracle. Both producer stages use LLVM only while regenerating the
asset; the resulting CTest has no runtime dependency on LLVM, NASM, FEX, K,
Python, or the native oracle. See the corpus README under
`tests/unit/x86_64/corpus/` for the canonical workflow and profile contract.

`masked-flags-subset.py` chooses the checked-in cases of the
`x86-64-masked-flags` tests from XE2's masked-flag request sets and the Xeon's
responses (`--all` writes every case for a whole-set check).
