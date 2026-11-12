# Instruction corpus sources

`source.py` generates the complete CPU correctness assets from the source
tables under `tests/unit/arm64/corpus/source`,
`tests/unit/x86_64/corpus/source`, and `tests/unit/x86_64/masked_flags/source`.
CMake owns materialization into each build tree. Generation uses Python's
standard library and the checked-in producers; it needs no Git database,
network, assembler, upstream checkout, execution engine, or reference hardware.
The unit binaries consume only their generated asset.

## Source tables

Each `index.json` explicitly lists its inputs and fixes record order. JSON
lines files group programs and golden expectations by producer. Bounded
`$table` loops, record, state and memory defaults (`$omit` removes a default
field), and byte and state recipes (`$runs` among them) factor regular data;
irregular columns remain literal values. [`loops.py`](loops.py) holds the
expression operators and [`source.py`](source.py) the recipes. The tables are
test inputs, not engine output, and recipes describe test data only: they never
decide instruction semantics.

The versioned manifests, coverage and provenance stay beside the source
tables. Generation must reproduce their accepted asset SHA-256 before
publishing an output. Updating native expectations still requires the
independently observed hardware results described by each corpus README.

The AArch32 corpus (`--kind aarch32`, `tests/unit/arm64/aarch32_corpus/source`)
has no pack to import: `tools/arm64-tests/a32-corpus.py` writes its source
tables from oracle runs, and `aarch32.py` generates the pack from them. Its
README owns the format.

## Promoting producer output

```sh
python3 -B tools/instruction-corpus/source.py --kind arm64 \
  --import-pack OUTPUT/a64_instruction_corpus.pack \
  --source tests/unit/arm64/corpus/source
```

Use `x86` or `masked-flags` for the other assets and their owning source
directory. The import also carries the producer's manifests and provenance.
Commit the source tables and metadata, never the generated pack or binary.
