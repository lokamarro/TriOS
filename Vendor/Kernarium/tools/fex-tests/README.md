# FEX instruction conformance corpus

The x86 gadget conformance campaign consumes the handwritten x86-64 and i386
assembly tests from the pinned `x86-conformance` dependency, for which FEX is
a test input only. The macOS dependency prefix builds FEXCore from the same
row for the macOS-only x86-64 JIT; see
[`dependencies/README.md`](../../dependencies/README.md).

Prepare the exact upstream source on a case-sensitive file system (the pinned K
semantics source tracks files whose names differ only by case, and
`prepare.sh` refuses a case-insensitive root):

```sh
dependencies/prepare.sh --product x86-conformance \
  --root "$CASE_SENSITIVE_DEPENDENCY_ROOT"
```

Then materialize a corpus and run it through the test-only gadgets adapter
(`tools/fex-tests/build-nasm.sh` builds the pinned NASM producer when none is
available, and `UML_FEX_NASM` names it off `PATH`):

```sh
python3 tools/fex-tests/build-asm-corpus.py \
  "$CASE_SENSITIVE_DEPENDENCY_ROOT/fex" OUTPUT_DIRECTORY --mode x86-64   # or i386
cmake --build BUILD_DIRECTORY --target uml_x86_64_fex_asm_runner
python3 tools/fex-tests/run-asm-corpus.py \
  BUILD_DIRECTORY/bin/uml_x86_64_fex_asm_runner OUTPUT_DIRECTORY REPORT.json
```

The output is disposable build data; `export-asm-source.py` reduces a prepared
checkout to a content-verified export when it must cross a machine boundary.
The producer rejects a dirty or unpinned FEX checkout, so a prepared source
carrying a maintained patch series yields the pinned corpus. The
`x86-64-fex-asm-campaign-runner` tooling test keeps the runner's output format
and the campaign in step.

The campaign executes every selected binary, including upstream-disabled
inputs, but excludes those from the applicable pass rate. It never filters
tests by host CPU features: those annotations describe guest instructions the
gadgets backend must implement. Some FEX expectations are FEX's own behaviour
where x86 hardware differs; the report gives the raw pass rate and the rate
under the policy that sets aside upstream's `Known_Failures_host` and the
project's [`contradicted-expectations.tsv`](contradicted-expectations.tsv), each
row with its native Intel evidence (`run-asm-corpus.py` documents the policy).
The engine emulates x87 only at full 80-bit precision, so the campaign removes
FEX's reduced-precision request from the configuration it hands the runner.
