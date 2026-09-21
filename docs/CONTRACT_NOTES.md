# Seer-Properties Implementation Contract Notes

Verified against the reference host repository (the private Seer host checkout
the plugins were developed and tested against). The local host checkout sat one
commit ahead of the reference revision, and that commit was documentation-only
(a docs folder, a relnote and a stats script), so the plugin runtime sources
were identical.

## Host Property pipeline (verified against the reference host source)

- `ProcessRunner::locateOutputFile` discovers the result file via
  `<output-base>.*` in the request directory, newest first. `${output_file}`
  expands to `<requestDir>/<md5-of-input-path>` with no extension, so the helper
  must publish `<output-base>.json`.
- `ProcessBackend` sets `cleanupOutputDirectoryOnFailure = true` for canonical
  requests: a nonzero exit code makes the host wipe the request directory, so a
  failed attachment never leaves a misleading partial result behind.
- Attachments are resolved relative to the request directory and loaded into
  `QImage` before the request directory is removed
  (`propertycapabilityhandler.cpp` `parseSchema1` + `onBackendFinished`).
  `resolveAttachment` rejects any reparse point in the path and requires the
  canonical path to be strictly inside the request directory itself.
- Images are supplied as `{"type":"image","value":"rel.png"}` data fields, NOT
  through an invented `images[]` field.
- A subgroup is `{"<Title>": {"value": { ...flat rows... }}}`. `parseSchema1`
  accepts flat string/scalar/array values inside `value` and **drops** anything
  else with a warning, so subgroup rows must be flat strings. Only `image` is a
  realised typed value; a `{"type":"text"}` row is degraded to text with a
  warning and is first run through `resolveAttachment`, so text rows are
  published as plain strings instead (the image plugin's `Histogram Y Scale`
  row was converted from the pre-existing typed form).
- Typed chart rows are legal inside a subgroup: `parseSchema1` resolves a
  `{"type":"image"}` field found in a subgroup, `PropertyImage::groupKey` carries
  the subgroup title to `InspectorController`, and the controller appends the
  chart to that subgroup (`Subgroup::charts`, rendered by `InspectorSubgroup`).
  This is a host change made for `image-histogram-property` 1.4.0: a host built
  before it hits `isFlatValue` for the nested row and drops the chart with a
  warning, so the statistics survive but the charts disappear. `parseSchema0`
  has no attachment handling at all, so a schema-0 plugin cannot publish charts
  anywhere.
- Both nesting levels accept the same value shapes: a flat
  string/number/bool/array becomes a row, `{"type":"image"}` becomes a chart, and
  any other typed field is published as the text it carries together with a
  warning - a value is never reported at one level and dropped at the other.
- A subgroup's `value` is either an object (rows render in the object's key
  order) or an **array of one-key fields** (rows and charts render in the
  plugin's own order - the array position is the sequence, and there is no
  separate `order` field; an earlier `order` design was removed before release).
  Each array item should be a single-key object; an item that is not an object or
  carries no fields is reported and skipped, and repeated keys across items
  resolve to one row node with the last published value winning. A host built
  before the change collapses the array into a single empty text row.
- One group per plugin: every package publishes its whole result - rows and
  charts - inside a single subgroup titled after the plugin (`Git`,
  `Image Scopes`, `Digital Signature`), so the Inspector shows one section per
  plugin instead of loose rows that can collide across plugins. Row identity is
  `stableEntryId(key, parentGroupOrSubgroupId)`, so the subgroup also namespaces
  the keys. `sha256-property` keeps the flat single-row form because its whole
  result is one row (user decision, 2026-09-21).
- `result_schema` must be exactly integer `1` for Property
  (`manifestinvocationparser.cpp`); `no_cache` is only valid for Preview.
- Property capability tokens allowed: `${input_file}`, `${output_file}`,
  `${output_dir}` (plus seer_dir/exe/7z). `${no_cache}` is forbidden.
- Matcher tokens: `${type_folder}`, `${type_file}`, `${type_all}`; matcher
  tokens are only valid for Control or Property capabilities.
- Top-level manifest keys are restricted; unknown keys only warn. The plugin id
  must match `^[a-z0-9]([a-z0-9_-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9_-]*[a-z0-9])?)+$`,
  so `io.1218.seer.git-info` and `io.1218.seer.digital-signature` are valid.
  Only `appMinVersion` is validated as a three-segment version string;
  `version` is read but not validated by the host.
- `invocations` present => canonical execution contract. The flat
  `command`/`arguments` shape (reference implementation: the current,
  actively maintained `sha256-property`) is a different, equally supported
  contract form, not a leftover; new plugins declare `invocations`.
- The helper's working directory is the package install directory, and
  `useCommandWrapper` is enabled by the host.
- The host may kill the helper and its whole process tree on timeout/cancel
  (`killChildTree`). Internal query deadlines must stay below the manifest
  timeout.
- Inspector row identity is `stableEntryId(key, parentGroupOrSubgroupId)`, not
  namespaced by plugin id. Distinct subgroup titles (`Git`, `Digital
  Signature`) and uniquely named text rows avoid collisions; image items become
  chart items keyed by their title.

## Output shapes required

Result groups (namespaced subgroup values, flat strings):

```json
{"result_schema":1,"data":{"Git":{"value":{"Repository":"not"}}}}
{"result_schema":1,"data":{"Digital Signature":{"value":{"Status":"Unsigned"}}}}
```

Image scopes (three peer image fields):

```json
{"type":"image","value":"rgb-histogram.png"}
{"type":"image","value":"waveform.png"}
{"type":"image","value":"vectorscope.png"}
```

## Plugin policy

- Canonical v1 process plugins only. `backend=process`, `capability=property`,
  `result_schema=1`. No `no_cache`.
- CLI shape: `--input <path> --output <output-base> --output-dir <request-directory>`
- UTF-8 JSON via a real serializer (nlohmann/json vendored single header, or
  `QJsonDocument` in the Qt-based image plugin). Never concatenate raw values.
- Only write inside the request directory; validate canonical containment and
  reject reparse-point escapes; never write next to the inspected file.
- Publish attachments first, single result JSON last, atomically.
- Exit 0 + valid JSON for expected domain outcomes (unsigned, git absent, not a
  repo, inaccessible, bounded timeout with an informative result). Nonzero only
  for malformed invocation, output publication failure, or no meaningful result.
- Distinct attachment names must not collide with `<output-base>.*` discovery.
- Internal deadlines stay below the manifest `timeout_ms`: the host may kill the
  whole process tree, so an abandoned query must still publish a bounded result.
  Uninterruptible work (Windows trust APIs, Qt image decoders) is either run on
  a worker thread with a timed wait or checked at step boundaries.
- Shared helpers live in `plugins/common/propertycommon.h` (header-only, since
  each package is a standalone CMake project). Containment, canonicalization,
  the `.json` suffix and atomic JSON publication must not be re-derived per
  package: the per-package copies had already drifted.

## Windows-specific findings

- `CreateProcessW` rejects an empty `lpCurrentDirectory`; pass `nullptr` when no
  working directory is needed.
- `CERT_FIND_SUBJECT_CERT` matches the issuer *and* serial number of a
  `CERT_INFO`; passing only the issuer blob finds nothing.
- `WinVerifyTrust` with `WTD_CHOICE_FILE` does not consult the system catalog.
  Catalog-signed files need the explicit
  `CryptCATAdminCalcHashFromFileHandle` / `CryptCATAdminEnumCatalogFromHash` /
  `WTD_CHOICE_CATALOG` path.
- An Authenticode signature must terminate the file: appending bytes makes
  Windows report "no signature", while flipping a byte inside the hashed range
  reports `TRUST_E_BAD_DIGEST`. Both are covered by fixtures.
- Git reports `--git-common-dir` relative to the current directory while
  `--absolute-git-dir` is absolute; resolve relative paths against the inspected
  directory before comparing them.
- `WinVerifyTrust` read failures surface as `CRYPT_E_FILE_ERROR` (0x80092003) or
  as wrapped Win32 codes (`ERROR_FILE_NOT_FOUND`, `ERROR_PATH_NOT_FOUND`,
  `ERROR_ACCESS_DENIED`, `ERROR_SHARING_VIOLATION`, `ERROR_LOCK_VIOLATION`).
  They mean "the file could not be read", not "the signature is malformed".
- `GetFullPathNameW` needs a buffer sized for the result; a fixed `MAX_PATH`
  buffer silently rejects longer paths (a real risk for request directories).
- A child process created with `CREATE_SUSPENDED` can be assigned to a Job
  Object before it spawns descendants; `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`
  then reaps the whole tree without the pid-reuse race of a toolhelp32
  parent-pid scan.

## Toolchain

- Plugins build standalone with CMake + CTest.
- `git-info-property` / `digital-signature-property`: MSVC x64, static CRT,
  no Qt.
- `image-histogram-property`: Qt 6.8 MSVC x64 (`<qt-prefix>`), deployed with
  `windeployqt`.
- No machine is assumed to ship a standalone Git: the real-git integration
  test discovers `git.exe` from `PATH` or known install locations (including
  the Visual Studio Team Explorer bundling) and reports SKIP when none is
  found.

## Release prerequisites

- Every manifest keeps `appMinVersion: "4.5.10"` for now. Releasing any
  package whose subgroup value is an **array of one-key fields**
  (`git-info-property`, `digital-signature-property`,
  `image-histogram-property`) requires a host release that carries the
  subgroup-array support: an older host collapses such a group into a single
  empty text row. `appMinVersion` in those manifests must be raised to the
  first host release that contains the support, in the same step that
  publishes them.
