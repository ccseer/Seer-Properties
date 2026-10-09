# PDB Property Plugin

This standalone package is a Canonical v1 process plugin that performs a
read-only summary query for `.pdb` debug symbol files. Its manifest declares a
Canonical Property invocation using `result_schema: 1` and produces a "PDB"
subgroup describing the examined file: format version, GUID, target
architecture, symbol/type/source counts and the opt-in largest-object
analysis.

The package interface is defined by `plugin.json`:

- backend: `process`
- capability: `property`
- extensions: `pdb`
- command: package-relative `pdb_property.exe`
- arguments: `--input ${input_file} --output ${output_file} --output-dir ${output_dir}`
- output: `${output_file}.json` with `result_schema: 1`

The inspected file is only ever opened read-only through a file mapping; it is
never loaded, executed or written to.

## What it checks

This plugin is based on
[raw_pdb](https://github.com/MolecularMatters/raw_pdb), the C++11 PDB/MSF
parsing library by Molecular Matters. The library is vendored at
[plugins/third_party/rawpdb](../third_party/rawpdb), pinned to main @
`43cc59b` (2026-06-24; the upstream publishes no release tags, so the commit
is pinned), licensed BSD 2-Clause — see the vendored
[README](../third_party/rawpdb/README.md) for the full provenance and the
integration caveats. The package adds validation, presentation and failure
containment around it:

- The MSF super block is validated before anything else. A file without the
  PDB magic is `NotAPdb`; a file smaller than its own header claims or with a
  broken free-block map is `CorruptPdb`. Both are published as informative
  results with exit 0 so the Inspector shows "this is not a valid PDB" rather
  than "the plugin failed".
- The info stream supplies the format version, build timestamp, age, GUID and
  the FASTLINK detection: a PDB produced with `/DEBUG:FASTLINK` is classified
  `FastLink` and a `Note` row explains that the detailed private records live
  in the linked OBJ files, not in this PDB.
- The DBI stream supplies the target machine, the module count, the public and
  global symbol record counts, the section-contribution count and the source
  file count. Missing optional streams leave their rows out instead of
  becoming zeros.
- The TPI and IPI counts come from the stream headers only, so they stay
  cheap even for multi-GB PDBs. Tier 1 (the default summary) never
  materializes the symbol record stream, which is the only structure that
  reaches hundreds of megabytes on huge PDBs.
- `--top N` (1–100) is the opt-in tier-2 analysis: the section-contribution
  stream is aggregated per module and the N largest contributors are reported
  with deterministic ordering (size descending, module index ascending on
  ties).
- raw_pdb reports malformed stream data through `PDB_ASSERT`, which compiles
  to a no-op in release builds, so a crafted file could cause an access
  violation inside the parser. The query therefore runs behind a
  structured-exception guard, and a fault is published as `QueryError`
  instead of crashing the helper.
- A file mapping is kept open with read/write sharing, so a debugger or
  linker holding the PDB does not block the query. Read failures (missing
  file, access denied, sharing violation) are `ReadError` results.
- The optional `--deadline <ms>` option bounds the query internally: an
  exhausted budget keeps every row gathered so far, appends a `Note` row
  naming the step where the analysis stopped, and still publishes the result.
  Without the option the host `timeout_ms` of 300000 governs — chosen for
  multi-GB PDB files.
- `--deadline 0` is the test hook shared with the other property packages:
  the budget is already exhausted, so the helper abandons the query at the
  first checkpoint and publishes `QueryError` with exit 0.

## Invocation

`--input`, `--output` and `--output-dir` are required; `--top` and
`--deadline` are optional. Every option consumes the next token; a repeated
option, an unknown option, an empty value, a trailing option without a value,
or a `--top` outside 1–100 is rejected with a nonzero exit code and no output.

## Result rows

All rows are published inside one `PDB` subgroup whose value is an
**array of one-key fields in the plugin's own order** (an object would impose
key order), so the Inspector renders them in the sequence below.

**Release prerequisite.** `appMinVersion` is still `4.5.10`, which predates the
host's subgroup-array support. It must be raised to the first host release that
carries it before this plugin is published; on an older host the whole subgroup
collapses into a single empty text row.

| Row | Meaning |
|---|---|
| `Status` | `Valid`, `FastLink`, `NotAPdb`, `CorruptPdb`, `ReadError`, `QueryError` |
| `Reason` | concise reason for a non-successful status |
| `Link Type` | `Full` or `/DEBUG:FASTLINK` |
| `Note` | FASTLINK explanation or partial-result note |
| `File Size` | 1024-based human-readable size |
| `PDB Format` | toolset name, e.g. `VC70`, `VC140` |
| `Built` | info-stream signature as UTC text |
| `Age`, `GUID` | info-stream identity |
| `Machine` | `x86`, `x64`, `ARM`, `ARM (Thumb)`, `ARM64`, `unknown` or raw hex |
| `Streams`, `Modules` | MSF stream and module counts |
| `Public Symbols`, `Global Symbols` | symbol record counts |
| `Types (TPI)`, `Inlinees (IPI)` | type record counts |
| `Source Files`, `Contributions` | per-module summary counts |
| `Line Info` | `Yes` or `No`, from the per-module line-stream flags |
| `Largest Object 1..N` | `module name (aggregated size)`, only with `--top` |

Rows are flat strings inside the subgroup; the reference host drops anything
deeper.

## Build and test

Dependencies: the C++ standard library, the vendored
[raw_pdb](https://github.com/MolecularMatters/raw_pdb) parsing engine
(source-vendored under `plugins/third_party/rawpdb`, see "What it checks"
above), the vendored single-header
[nlohmann/json](../third_party), the shared
[propertycommon.h](../common/propertycommon.h) (path containment, `.json`
suffix, atomic JSON publication), and the Win32 API. No Qt. The helper links
the static CRT, so the built executable is self-contained.

```powershell
cd plugins/pdb-property
cmake --preset default            # Ninja, Release; binaryDir under C:/Dev/build_output/Seer-Properties
cmake --build --preset default
ctest --preset default
```

The test suite:

- `pdb_property_format_test` — pure formatting helpers, no fixtures.
- `pdb_property_query_test` — the real parser against the committed fixtures
  in `test/fixtures` (a full PDB and a FASTLINK variant, see
  [fixtures/README.md](test/fixtures/README.md) for their provenance), plus
  missing/garbage/truncated samples and the zero-budget behavior.
- `pdb_property_helper_test` — CLI parsing rules, query-function injection,
  the zero-deadline hook and the published JSON shape.
- `pdb_property_manifest_test` — manifest contents plus the staged helper run
  against a non-PDB sample.

## Packaging

Run `scripts/package-plugins.ps1` from the repository root: it builds every
package, runs its tests, installs the flat package tree, and writes one ZIP
per package `plugins/<pkg>/<pkg>-<version>.zip`. The archive contains exactly
one package-root `plugin.json`, the helper executable and `README.md` (staged
from `PACKAGE_README.md`); no build or test tree is included.

## Windows baseline

Validated on Windows 10 / Windows 11 x64. The APIs used (`CreateFileW`,
file mappings and the structured-exception guard) exist since the earliest
supported Windows versions; where evidence is unavailable the helper publishes
a valid result describing the limitation instead of crashing.
