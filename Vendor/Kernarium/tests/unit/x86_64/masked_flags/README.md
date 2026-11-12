# x86 masked floating-point flags

`source/` holds the instruction encodings and the selected operand sets with
the flags observed on the Intel Xeon. The
`uml_x86_64_masked_flags` CMake target materializes `xeon-flags.bin` in the
build tree, preserving its accepted SHA-256, for both engine consumers.
Generation needs neither a Xeon nor XE2's original requests or responses.

`tools/x86-tests/masked-flags-subset.py` produces an updated selection from
the independent Xeon observations and the engine responses used only for
selection. Promote an accepted selection with the source contract's import
command and `--kind masked-flags`. Commit its source tables and provenance,
not the binary. The
[source contract](../../../../tools/instruction-corpus/README.md) owns the
recipe format and materialization.
