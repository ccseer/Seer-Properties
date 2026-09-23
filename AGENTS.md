# AGENTS.md

## 1. Project Identity

Seer-Properties hosts the official property plugins for
[Seer](https://1218.io/). Every package here is an independently installable
Canonical v1 process plugin. Each deployed package is flat under the
configured installation root: `<installRoot>/<package>/plugin.json`, the
helper executable, and whatever runtime assets that helper needs.

Each package is a standalone CMake project with its own CTest suite. There is
no top-level build system; always build and test a single package directory.

## 2. Directory Structure

```
plugins/<package>/
  plugin.json     manifest consumed by the host
  src/            helper sources
  test/           unit, contract and integration tests
  CMakeLists.txt  standalone CMake project with CTest tests
  README.md       per-package interface, limits, build and packaging steps
  PACKAGE_README.md  shipped user-facing readme: what the plugin does and every
                     option its helper accepts. Staged and installed as
                     README.md, so it is the copy that lands in the ZIP.
plugins/common/propertycommon.h    header-only helpers shared by the non-Qt
                                   packages (path containment, .json suffix,
                                   atomic JSON publication)
plugins/common/PackageStaging.cmake   shared CMake function that stages and
                                   installs plugin.json, the helper and
                                   README.md into one flat package root
plugins/third_party/nlohmann/json.hpp   single-header JSON (build-time only)
docs/CONTRACT_NOTES.md   host-alignment findings; read before touching helpers
docs/RESULTS.md          verification report for the current plugin set
```

Every package is a standalone CMake project, so shared code is header-only and
added to `target_include_directories` as `../common`. Shared behaviour that
must not drift between packages:

- Path containment and atomic JSON publication: `propertycommon.h`
  (`isContainedInDirectory`, `withJsonSuffix`, `writeJson`, `absolutePath`,
  `stripTrailingSeparators`). The Qt-based image package keeps QString
  equivalents because it does not include the Win32/JSON header.
- CLI parsing rules: every option consumes the next token; a repeated,
  unknown, empty-valued or trailing option without a value is an invocation
  error (nonzero, no output). The loops stay per-package because the option
  sets differ (`--git`, `--scopes`), but the rules are identical.
- Internal-deadline parameter convention: a positive value is the budget in
  milliseconds; `0` means "the budget is already exhausted", so the work is
  abandoned at the first checkpoint. No value disables the deadline. Both
  `digital-signature-property` and `image-histogram-property` follow this, so
  `0` is a test hook everywhere and never means "run without a deadline".
- Output shape: every package publishes its whole result - flat rows and typed
  chart fields alike - inside exactly one subgroup titled after the plugin
  (`Git`, `Image Scopes`, `Digital Signature`), so the Inspector shows one
  section per plugin instead of loose rows that could collide across plugins.
  The subgroup's `value` is an **array of one-key fields** and the array order is
  the render order. A package whose whole result is a single row
  (`sha256-property`) keeps the flat form. Charts inside a subgroup need a host
  that resolves typed values there (the 2026-09-21 host change); a host built
  before it collapses the array into a single empty text row, so the owning
  package's `appMinVersion` has to be raised to the first host release that
  carries the change.

Current packages: `image-histogram-property`, `sha256-property`,
`git-info-property`, `digital-signature-property`.

## 3. Critical Rules

- Preserve the Canonical v1 contract: `backend: "process"`,
  `capabilities: ["property"]`, `result_schema: 1` (must be the integer `1`
  for Property), `invocations` present. The flat
  `command`/`arguments` CLI form exists only in the reference
  `sha256-property` package; new packages declare `invocations`. All property
  packages are current, actively maintained work — there are no legacy
  property packages.
- Manifest invariants: `schema_version` is `1`; `id` matches
  `^[a-z0-9]([a-z0-9_-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9_-]*[a-z0-9])?)+$`;
  `version` and `appMinVersion` are three-segment strings. Keep the package
  `id` stable across versions.
- CLI shape: `--input <path> --output <output-base> --output-dir
  <request-directory>`. The host expands `${output_file}` to
  `<requestDir>/<md5-of-input-path>` with no extension, so the result must be
  published as `<output-base>.json`.
- Publish attachments first and the single result JSON last, both
  atomically. Attachment names must not collide with the `<output-base>.*`
  discovery pattern.
- Host-directed output — the result JSON and every attachment — is written
  only inside the host-provided request directory; validate canonical
  containment and reject reparse-point escapes. Never write next to the
  inspected file. Helper-initiated temporary files instead follow the
  location policy in Section 4.
- Exit `0` with valid schema-1 JSON for every expected domain outcome
  (unsigned, git absent, not a repo, inaccessible, bounded timeout with an
  informative result). Exit nonzero only for malformed invocation, output
  publication failure, or no meaningful result.
- Produce UTF-8 JSON with a real serializer (nlohmann/json or
  `QJsonDocument`). Never concatenate raw values into JSON text.
- The host may kill the helper and its whole process tree on timeout or
  cancel. Internal query deadlines must stay below the manifest
  `timeout_ms`.
- Schema-1 subgroup values must be flat strings; the host drops anything
  else inside a subgroup `value` object. Only `type: "image"` is a realised
  typed value.
- **Shipped package README.** Every package owns a `PACKAGE_README.md`: one
  short paragraph describing what the plugin reports, followed by an
  "Options & Arguments" table covering every option the helper accepts (option,
  accepted values, default, purpose). `seer_property_package_staging` in
  `plugins/common/PackageStaging.cmake` stages it and installs it as
  `README.md` next to `plugin.json`, so it is packed into the distributable
  ZIP, and each `*_manifest_test` asserts the staged copy exists and is
  non-empty. Update `PACKAGE_README.md` in the same change whenever the CLI
  surface moves — an option added, removed, renamed, re-defaulted, or given a
  different set of accepted values — and whenever the user-visible behavior
  changes. The developer `README.md` is not shipped and never substitutes for
  it.
- Comments, identifiers and log strings are English throughout.

## 4. Temporary and Intermediate File Location Policy

This policy covers every file a helper creates on its own initiative
(scratch, staging, cache, intermediate render output).

**Exception — where the host directs otherwise.** When the host program Seer
explicitly specifies a location, the host wins: the final result JSON and all
attachments are host-directed output and must always be published into the
host-provided request directory (`${output_file}` / `${output_dir}`), and
only there (see Section 3). This includes the staging file used to publish
them atomically, which must be created in the same directory (same volume) as
its destination so the final rename stays atomic. The preference order below
applies to everything the helper decides about by itself.

For helper-initiated temporary files, follow this strict priority order:

1. **Preferred: the plugin executable's directory** — the package directory
   that contains `plugin.json` and the helper executable (which is also the
   host-provided working directory). Resolve it from the helper executable's
   own module path (`GetModuleFileNameW` on itself), never from the current
   directory of a child process or a hard-coded install root.
2. **Fallback: the system temporary directory** (`GetTempPathW`) — use it
   only when the plugin executable's directory is not writable: read-only
   install location such as `Program Files`, ACL denial, disk full, or a
   locked directory. Probe writability by creating and deleting a uniquely
   named probe file in that directory, decide once per process, and reuse
   the decision for the lifetime of the process.
3. **Never** write temporary files next to the inspected file, and never
   invent other locations (per-user profile directories, fixed paths,
   drive roots).
4. Always clean up every file created under the plugin executable's
   directory before exit; never leave scratch files behind next to
   `plugin.json`. Remove files created in the system temporary directory on
   normal exit too — the host's failure cleanup only covers the request
   directory and cannot reach `%TEMP%`.

## 5. Build, Test, Package

Run commands from inside the package directory (substituting the package
name). Reference packages build with MSVC x64 and the static CRT (C++
standard library + Win32 + vendored nlohmann/json only);
`image-histogram-property` additionally uses Qt 6.8 Core/Gui and needs
`CMAKE_PREFIX_PATH` pointing at the Qt install (the local preset supplies
it) plus `windeployqt` at packaging time.

A machine-local `CMakeUserPresets.json` in every package directory points
the build out of the repository, to
`C:/Dev/build_output/Seer-Properties/<name>` (Ninja, Release).
The Ninja generator needs the MSVC environment on PATH (see
`<build-root>/msvcenv.sh`); the preset file is
machine-local and gitignored, so on a machine without it configure with an
explicit out-of-repo `-B` directory instead.

- Language standard: C++17 minimum. Do not require C++20 or later features.
- Target OS: Windows 10 or later, x64 only. Do not use Win32 APIs that are
  unavailable on Windows 10, and do not add Windows 7/8 compatibility paths.

```powershell
# Configure + build (sha256 example; substitute the package name)
cd plugins/sha256-property
cmake --preset default            # Ninja, Release, out-of-repo binaryDir
cmake --build --preset default

# Test (full suite)
ctest --preset default

# Package every plugin (orchestrates configure/build/test/install/archive)
cd <repo root>
.\scripts\package-plugins.ps1 -BuildRoot "<build-root>" `
                              -QtDir "<qt-prefix>" `
                              -SevenZip "C:\Program Files\7-Zip\7z.exe"
# A single package by hand (maximum compression level)
cmake --install "<build-root>/sha256-property" --prefix dist
& "C:\Program Files\7-Zip\7z.exe" a -tzip -mx=9 -y sha256-property-<version>.zip .\dist\*
```

The script writes `plugins/<pkg>/<pkg>-<version>.zip` (version from
`plugin.json`) at maximum compression, verifies every archive entry against
`dist`, and prints the SHA-256. It writes **no** `*.zip.sha256` sidecar: the
archive is verified at build time, so a committed sidecar could only go stale.

- Test targets follow `<helper>_*_test` naming (for example
  `sha256_property_manifest_test`, `git_info_output_test`,
  `image_histogram_scope_renderer_test`). Every new test is registered with
  `add_test` and keeps the existing suite green.
- The `*_manifest_test` suites stage `plugin.json` plus the helper and verify
  the manifest contract end-to-end; extend them whenever the manifest or CLI
  surface changes.
- Verify the final ZIP layout before distributing: exactly one package-root
  `plugin.json`, the helper, `README.md` (from `PACKAGE_README.md`), required
  runtime assets, no build/test tree, no accidental outer directory.
- Package ZIPs at the maximum compression level: `7z a -tzip -mx=9` (7-Zip), or
  `-CompressionLevel Optimal` for `Compress-Archive` (equivalent to .NET
  `CompressionLevel::Optimal`). Never ship `Fastest`/`NoCompression` (store)
  archives; if a packaging tool lacks a compression switch, switch tools
  rather than lower the level. `scripts/package-plugins.ps1` prefers 7-Zip when
  it is installed (`-SevenZip <path>`) and falls back to
  `System.IO.Compression` otherwise.
- Archives are build output and are gitignored (`*.zip`, `dist/`). Never commit
  an archive or a checksum file for one. A successful packaging run deletes the
  package's other archives, so a manual install cannot pick a stale version.
- Bump the package `version` in `plugin.json` when behavior changes, and
  update that package's `README.md` and `PACKAGE_README.md` in the same change.
  Extend `docs/CONTRACT_NOTES.md` (never contradict it) when a host-alignment
  finding is discovered.

## 6. Dependency Policy

- `git-info-property` and `digital-signature-property`: C++ standard
  library (C++17), vendored nlohmann/json, Win32 API (Windows 10+) only.
  Static CRT, one self-contained executable each.
- `sha256-property`: Win32 BCrypt API (Windows 10+) only.
- `image-histogram-property`: Qt 6.8 (Core/Gui); `windeployqt` supplies the
  runtime assets at packaging time.
- Third-party libraries are allowed, but prefer libraries that can be
  statically linked or built from source (header-only or source-vendored),
  to keep the deployed package as small as possible and minimize shipped
  files. Avoid dependencies that require external runtime DLLs or separate
  installers; if a dynamic library is unavoidable, document its deployment
  in the package README and packaging steps.
- Do not introduce new third-party dependencies without updating this file,
  the package README, and the packaging steps.
