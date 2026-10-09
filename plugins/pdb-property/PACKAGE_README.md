# PDB Property

**PDB Property** is an official Canonical v1 Property plugin for [Seer](https://1218.io/), the file preview tool for Windows. It performs a read-only summary query for the currently previewed `.pdb` file and reports what the PDB contains: format version, GUID, target architecture, symbol/type/source counts and — optionally — which object modules contributed the most bytes.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <file>` | an existing `.pdb` file | required | The file to inspect. Populated automatically by Seer via `${input_file}`. |
| `--output <output-base>` | a path inside the request directory | required | Base path of the result; the helper publishes `<output-base>.json`. Populated automatically by Seer via `${output_file}`. |
| `--output-dir <request-directory>` | an existing directory | required | The host-assigned request directory; the result is only written inside it. Populated automatically by Seer via `${output_dir}`. |
| `--top <N>` | integer, 1–100 | disabled | Opt-in analysis of the section-contribution stream: reports the up to N object modules that contributed the most bytes as `Largest Object 1..N` rows. |
| `--deadline <ms>` | integer, 0–4294967295 | disabled | Internal budget in milliseconds for the query. When the budget is exhausted, every row gathered so far is still published and a `Note` row explains where the analysis stopped. Without this option no internal budget applies and the host `timeout_ms` (300000) governs. |

The manifest `arguments` can be extended in the host's plugin configuration to
turn an option on permanently, for example:

```json
"arguments": [
  "--input", "${input_file}",
  "--output", "${output_file}",
  "--output-dir", "${output_dir}",
  "--top", "10"
]
```

## Result

All rows are published inside one `PDB` subgroup; each row is a flat string.
Rows whose information is unavailable are omitted rather than guessed.

| Row | Meaning |
|---|---|
| `Status` | `Valid`, `FastLink`, `NotAPdb`, `CorruptPdb`, `ReadError`, `QueryError` |
| `Reason` | concise explanation for a non-successful status |
| `Link Type` | `Full` or `/DEBUG:FASTLINK` |
| `Note` | FASTLINK explanation, or where a deadline stopped the analysis |
| `File Size`, `PDB Format`, `Built`, `Age`, `GUID`, `Machine` | file and info-stream metadata |
| `Streams`, `Modules` | MSF stream and module counts |
| `Public Symbols`, `Global Symbols` | symbol record counts |
| `Types (TPI)`, `Inlinees (IPI)` | type record counts |
| `Source Files`, `Contributions`, `Line Info` | per-module summary |
| `Largest Object 1..N` | `module name (aggregated size)`, only with `--top` |

A file that is not a PDB, is truncated or is a FASTLINK PDB is reported as an
informative result with exit code 0 — it is a domain outcome, not a plugin
failure.
