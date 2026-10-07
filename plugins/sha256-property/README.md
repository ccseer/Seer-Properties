# File Hashes Property

This standalone package is a Canonical v1 process plugin that computes file
hash properties for every regular file, including extensionless and Unicode
paths. Its manifest uses the host `${type_file}` matcher and writes a schema-1
JSON property file beside the requested output base path. The default
selection computes SHA-256 only; further algorithms are opt-in through the
arguments, which keeps the default output identical to the historical
single-row shape.

The package interface is fixed by `plugin.json`:

- backend: `process`
- capability: `property`
- file matcher: `${type_file}`
- invocation: canonical `invocations.property` form with `result_schema: 1`
- command: package-relative `sha256_property.exe`
- arguments: `--input ${input_file} --output ${output_file}`
- optional arguments: `--case lower|upper`,
  `--algorithms <comma-separated list|all>`
- output: `${output_file}.json`, containing
  `{"result_schema":1,"data":{"SHA-256":"<64 hex characters>"}}` for the
  default selection (lowercase by default)

## Algorithms

Eight algorithms are supported. The canonical priority order below is the
fixed publish order for grouped output, regardless of the order in the argument
list. MD5 and SHA-1 are deliberately not offered: Seer already reports both by
default, so publishing a second copy of them would only duplicate existing
rows.

| Priority | Row label | CLI name | Implementation |
|---|---|---|---|
| 1 | SHA-256 | `sha256` | Windows CNG |
| 2 | CRC32 | `crc32` | CRC-32 (IEEE, zip/gzip compatible) |
| 3 | BLAKE3 | `blake3` | vendored reference implementation |
| 4 | SHA-512 | `sha512` | Windows CNG |
| 5 | xxHash | `xxhash` | XXH64 (vendored xxHash single header) |
| 6 | SHA-3-256 | `sha3-256` | FIPS 202 (vendored Keccak implementation) |
| 7 | CRC64 | `crc64` | CRC-64/XZ |
| 8 | SHA-384 | `sha384` | Windows CNG |

`--algorithms` accepts a comma-separated list of CLI names (case-insensitive,
surrounding spaces tolerated, duplicates collapse). The special list item
`all` selects every algorithm but must be the only item. An empty list item or
an unknown name is an invocation error (exit code 2, no output).

## Output shape

The helper always publishes the schema-1 envelope
`{"result_schema":1,"data":...}` through the vendored nlohmann/json. Selected
algorithm count decides the form:

- One algorithm (the default, or an explicit single name): one flat data row,
  for example `{"result_schema":1,"data":{"CRC32":"<8 hex characters>"}}`.
- Several algorithms: one subgroup titled `Hashes` whose value is an
  **array of one-key fields** in the canonical priority order, for example
  `{"result_schema":1,"data":{"Hashes":{"value":[{"SHA-256":"..."},{"CRC32":"..."}]}}}`.
  This is the ordered subgroup form; a host without subgroup-array support
  collapses it into a single empty text row (see below), while the default
  single-row output keeps working everywhere.

Digests of numeric checksums are published as fixed-width big-endian hex
(CRC32: 8 characters, CRC64 and xxHash: 16 characters) and follow `--case`
like every other algorithm.

## Case and algorithms configuration

The helper executable accepts the optional `--case lower|upper` argument
(defaulting to `lower`) and the optional `--algorithms <list|all>` argument
(defaulting to `sha256`). To configure them in Seer's plugin settings, override
the capability arguments, for example:

```json
["--input", "${input_file}", "--output", "${output_file}", "--algorithms", "all"]
```

Grouped output requires a host release that carries the subgroup-array
support. `appMinVersion` therefore has to be raised to the first host release
that contains the support when this package version is published; the default
single-row output itself has no such requirement.

Build and validate the package with:

```powershell
cd plugins/sha256-property
cmake --preset default            # Ninja, Release; binaryDir under C:/Dev/build_output/Seer-Properties
cmake --build --preset default --target sha256_property_manifest_test
ctest --preset default -R sha256_property_manifest_test
```

Create a distributable package with `scripts/package-plugins.ps1` from the
repository root (it writes one ZIP per package
`plugins/<pkg>/<pkg>-<version>.zip` and prints its SHA-256), or package by hand
from inside the package directory:

```powershell
cmake --install "C:/Dev/build_output/Seer-Properties/sha256-property" --prefix dist
& "C:\Program Files\7-Zip\7z.exe" a -tzip -mx=9 -y sha256-property-1.1.0.zip .\dist\*
```

No `sha256-property-1.1.0.zip.sha256` is produced: the script verifies every
archive entry against `dist` and prints the checksum, so a committed sidecar
file could only go stale.

The manifest test stages both `plugin.json` and the helper, checks the
manifest contract (canonical `invocations.property` invocation, package-relative
command, version, name), then invokes the staged helper to verify that a
`base` output produces `base.json` and that the default and grouped output
shapes are byte-exact. The unit test covers every algorithm against
independently generated reference vectors (Python hashlib/zlib and the
reference xxhash/blake3 packages), the argument parser, and the exact
published JSON for the default, single-algorithm, grouped, and `all`
selections.
