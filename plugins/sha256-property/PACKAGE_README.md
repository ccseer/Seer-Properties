# File Hashes Property

**File Hashes Property** is an official Canonical v1 Property plugin for [Seer](https://1218.io/), the file preview tool for Windows. It computes the digests of the currently previewed file. By default it reports SHA-256 only; other algorithms can be selected through the arguments.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <file>` | an existing regular file | required | The file to hash. Populated automatically by Seer via `${input_file}`. |
| `--output <output-base>` | a path inside the request directory | required | Base path of the result; the helper publishes `<output-base>.json`. Populated automatically by Seer via `${output_file}`. |
| `--case <case>` | `lower`, `upper` | `lower` | Letter case of the published hex digest. |
| `--algorithms <list>` | `sha256`, `crc32`, `blake3`, `sha512`, `xxhash`, `sha3-256`, `crc64`, `sha384`, or `all` | `sha256` | Comma-separated algorithms to compute, in any order (names are case-insensitive and duplicates collapse); `all` must be the only list item. Output rows are always published in the fixed order: SHA-256, CRC32, BLAKE3, SHA-512, xxHash, SHA-3-256, CRC64, SHA-384. MD5 and SHA-1 are not offered because Seer already reports both by default. |

## Output shape

- One selected algorithm: one flat row, for example `{"result_schema":1,"data":{"SHA-256":"<64 hex characters>"}}`.
- Several selected algorithms: one `Hashes` group whose rows appear in the fixed priority order above.

Algorithm variants: CRC32 is the standard CRC-32 (IEEE, as used by zip/gzip), CRC64 is CRC-64/XZ, xxHash is XXH64 (the `xxhsum` default), SHA-3-256 is the FIPS 202 algorithm, and BLAKE3 produces the standard 256-bit digest.

To configure the algorithms in Seer's plugin settings, override the capability arguments, for example:

```json
["--input", "${input_file}", "--output", "${output_file}", "--algorithms", "all"]
```
