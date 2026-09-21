# Digital Signature Property Plugin

This standalone package is a Canonical v1 process plugin that performs a
read-only Windows Authenticode query for `.dll` and `.exe` files. Its manifest
declares a Canonical Property invocation using `result_schema: 1` and produces
a "Digital Signature" subgroup describing the examined signature and the
verification outcome.

The package interface is defined by `plugin.json`:

- backend: `process`
- capability: `property`
- extensions: `dll`, `exe`
- command: package-relative `digital_signature.exe`
- arguments: `--input ${input_file} --output ${output_file} --output-dir ${output_dir}`
- output: `${output_file}.json` with `result_schema: 1`

The inspected file is never loaded, mapped or executed; only its headers are
read and the trust APIs are pointed at the path.

## What it checks

- `WinVerifyTrust` with the `WINTRUST_ACTION_GENERIC_VERIFY_V2` policy. Only a
  zero return value counts as trust-verification success; that return value is
  a status code, not an `HRESULT`.
- Embedded signature first (located through `IMAGE_DIRECTORY_ENTRY_SECURITY`),
  then a bounded **local** catalog-signature fallback:
  `CryptCATAdminAcquireContext` -> `CryptCATAdminCalcHashFromFileHandle` ->
  `CryptCATAdminEnumCatalogFromHash`, followed by a `WTD_CHOICE_CATALOG`
  verification against the single catalog context that enumeration returned.
  The lookup is deliberately bounded: one call to
  `CryptCATAdminEnumCatalogFromHash` yields the one catalog whose member list
  contains the hash, and only that catalog is verified. `WinVerifyTrust`
  with `WTD_CHOICE_FILE` does *not* consider the system catalog, so the lookup
  is explicit. Everything stays on the local machine: no catalog download, no
  network access.
  A file is only reported as "unsigned" after both the embedded and catalog
  cases have been considered.
- Multiple signatures: the number of signers in the embedded container is
  reported and the certificate metadata always states that it comes from the
  first signer. One signer's verdict is never generalized to all signers.
- Offline, cache-only verification by default: `WTD_UI_NONE`,
  `WTD_REVOKE_NONE` together with `WTD_CACHE_ONLY_URL_RETRIEVAL` (the revoke
  flag alone does not prevent network retrieval) and `WTD_DISABLE_MD2_MD4`.
  Revocation limitations are stated rather than converted into a verified
  "not revoked" conclusion.
- The trust-state handle is closed with `WTD_STATEACTION_CLOSE` on every path,
  including the failing ones.
- A before/after file-identity check reports a changed/unavailable result
  instead of mixing metadata from different file versions.
- The query runs on a worker thread under an internal deadline of 20000 ms,
  below the manifest `timeout_ms` of 30000. A query that does not finish is
  abandoned and reported as `QueryError` - never as `Unsigned` - so a result is
  always published before the host can kill the helper.
- A status that says the trust provider could not read the file
  (`CRYPT_E_FILE_ERROR`, or a wrapped `ERROR_FILE_NOT_FOUND` /
  `ERROR_ACCESS_DENIED` / `ERROR_SHARING_VIOLATION` / `ERROR_LOCK_VIOLATION`)
  is reported as `ReadError`, not as a verdict about the signature.

## Invocation

`--input`, `--output` and `--output-dir` are all required and each consumes the
next token; a repeated option, an unknown option, an empty value or a trailing
option without a value is rejected with a nonzero exit code.

## Result rows

All rows are published inside one `Digital Signature` subgroup whose value is an
**array of one-key fields in the plugin's own order** (an object would impose key
order), so the Inspector renders them in the sequence below.

**Release prerequisite.** `appMinVersion` is still `4.5.10`, which predates the
host's subgroup-array support. It must be raised to the first host release that
carries it before this plugin is published; on an older host the whole subgroup
collapses into a single empty text row.

| Row | Meaning |
|---|---|
| `Status` | `Valid`, `Unsigned`, `HashMismatch`, `UntrustedChain`, `Revoked`, `RevocationUnavailable`, `UnsupportedOrMalformed`, `ReadError`, `QueryError` |
| `Verification Scope` | the examined policy and its offline limitation |
| `Signature Source` | `embedded`, `catalog`, or `none` |
| `Signature Type` | container kind, e.g. `Authenticode PKCS#7 (embedded)` |
| `Signers Examined` | `1`, `first of N`, or `none` |
| `Catalog Signature` | outcome of the local catalog lookup |
| `Publisher`, `Issuer` | simple display names of the examined signer certificate |
| `Certificate Fingerprint` | hash algorithm plus value, SHA-256 preferred, SHA-1 fallback |
| `Signing Digest Algorithm` | friendly name of the signer digest OID |
| `Certificate Validity` | `NotBefore to NotAfter` |
| `Timestamp` | presence only; a signing time is never invented |
| `Reason` | concise reason/code for a non-successful verification |

Rows are flat strings inside the subgroup; the reference host drops anything
deeper. A valid signature is a cryptographic/trust result, never a
malware-safety verdict.

## Build and test

Dependencies: the C++ standard library, the vendored single-header
[nlohmann/json](../../third_party), the shared
[propertycommon.h](../common/propertycommon.h) (path containment, `.json`
suffix, atomic JSON publication — shared with `git-info-property`), and the
Win32 API. No Qt and no other third-party code. The helper links the static
CRT, so the built executable is self-contained.

```powershell
cd plugins/digital-signature-property
cmake --preset default            # Ninja, Release; binaryDir under C:/Dev/build_output/Seer-Properties
cmake --build --preset default
ctest --preset default
```

`digital_signature_output_test` also covers the internal deadline: an injected
query that outlives it publishes `QueryError` with exit 0.
`digital_signature_query_test` runs against real Windows files (a system
binary, a body-modified copy, an appended copy, and a non-PE sample) and
reports SKIP for a fixture that the machine cannot provide; it never installs a
trust root and never uses signing secrets. Deterministic status/output tests use
an injectable trust-query boundary instead.

## Packaging

Run `scripts/package-plugins.ps1` from the repository root: it builds every
package, runs its tests, installs the flat package tree, deploys the Qt runtime
for the image package, and writes one ZIP per package
`plugins/<pkg>/<pkg>-<version>.zip`. Every archive entry is verified against
the installed tree and the SHA-256 is printed; no sidecar checksum file is
written.

The archive contains exactly one package-root `plugin.json` and the helper
executable; no build or test tree is included.

## Windows baseline

Validated on Windows 10 / Windows 11 x64. The APIs used
(`WinVerifyTrust`, `CryptQueryObject`, the certificate store APIs and the
catalog API) exist since Windows 7; where evidence is unavailable the helper
publishes a valid result describing the limitation instead of crashing.
